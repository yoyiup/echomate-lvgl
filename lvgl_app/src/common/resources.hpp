/*
 * resources.hpp —— Linux 应用的 RAII 资源封装（C++17，零依赖）
 *
 * 【为什么需要这个头】
 *
 *   写 Linux 应用最常漏的不是逻辑，是资源：
 *     · open/close、fopen/fclose、popen/pclose
 *     · socket/close
 *     · mmap/munmap（munmap 还要记住当初的长度！）
 *     · malloc/free
 *     · pthread_create/pthread_join
 *
 *   这类 bug 的共同特点：
 *     ① 不会立刻报错，跑几分钟甚至几小时才出问题
 *     ② 一旦出问题，现象和资源本身毫无关系
 *        （参考本项目：cam_start 失败路径漏 free → 169 次 × 200KB
 *         → 内核 OOM 杀掉进程，界面上只显示"打开失败"）
 *     ③ 失败路径越多，越容易漏。N 个 return 就要写 N 段清理
 *
 *   RAII 把"释放"绑到对象生命周期上：只要构造成功，作用域结束必然释放。
 *   于是失败路径不需要写任何清理代码 —— return 就够了。
 *
 * 【设计约束】
 *   · **不抛异常**：板子是 uClibc，且大量 C 代码混编。异常穿越 C 边界
 *     是未定义行为。所有失败都用 valid() 判断 + 返回码。
 *   · **move-only**：资源不能复制，只能转移所有权。用 = delete 关掉拷贝。
 *   · **零开销**：只有一个字段，全部 inline，编出来和手写 C 一样。
 */

#ifndef RESOURCES_HPP
#define RESOURCES_HPP

#include <cstddef>
#include <cstdio>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <netdb.h>		/* getaddrinfo / freeaddrinfo */
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace res {

/* ------------------------------------------------------------------ */
/* 文件描述符：open / socket / dup / pipe 都适用                       */
/* ------------------------------------------------------------------ */
class Fd {
public:
	Fd() noexcept = default;
	explicit Fd(int fd) noexcept : fd_(fd) {}
	~Fd() { reset(); }

	Fd(const Fd &) = delete;
	Fd &operator=(const Fd &) = delete;

	Fd(Fd &&o) noexcept : fd_(o.release()) {}

	Fd &operator=(Fd &&o) noexcept
	{
		if (this != &o) {
			reset(o.release());
		}
		return *this;
	}

	int get() const noexcept { return fd_; }
	bool valid() const noexcept { return fd_ >= 0; }
	explicit operator bool() const noexcept { return valid(); }

	/** 放弃所有权，交给调用方管理（例如交给内核） */
	int release() noexcept
	{
		int f = fd_;

		fd_ = -1;
		return f;
	}

	void reset(int fd = -1) noexcept
	{
		if (fd_ >= 0)
			::close(fd_);
		fd_ = fd;
	}

private:
	int fd_ = -1;
};

/* ------------------------------------------------------------------ */
/* FILE*：fopen / fdopen 适用                                          */
/* ------------------------------------------------------------------ */
class File {
public:
	File() noexcept = default;
	explicit File(std::FILE *fp) noexcept : fp_(fp) {}
	~File() { reset(); }

	File(const File &) = delete;
	File &operator=(const File &) = delete;

	File(File &&o) noexcept : fp_(o.release()) {}

	File &operator=(File &&o) noexcept
	{
		if (this != &o) {
			reset(o.release());
		}
		return *this;
	}

	std::FILE *get() const noexcept { return fp_; }
	bool valid() const noexcept { return fp_ != nullptr; }
	explicit operator bool() const noexcept { return valid(); }

	std::FILE *release() noexcept
	{
		std::FILE *p = fp_;

		fp_ = nullptr;
		return p;
	}

	void reset(std::FILE *fp = nullptr) noexcept
	{
		if (fp_)
			std::fclose(fp_);
		fp_ = fp;
	}

private:
	std::FILE *fp_ = nullptr;
};

/**
 * popen 的管道。
 *
 * ★ 和 File 分开的原因：popen 必须用 pclose 关闭，用 fclose 关会
 *   漏掉 waitpid —— 子进程变僵尸，FILE 也泄漏。
 *   写错时编译器不报，只会在 ps 里看到一堆 <defunct>。
 */
class Pipe {
public:
	Pipe() noexcept = default;
	explicit Pipe(std::FILE *fp) noexcept : fp_(fp) {}
	~Pipe() { reset(); }

	Pipe(const Pipe &) = delete;
	Pipe &operator=(const Pipe &) = delete;

	Pipe(Pipe &&o) noexcept : fp_(o.fp_), rc_(o.rc_)
	{
		o.fp_ = nullptr;
		o.rc_ = -1;
	}

	Pipe &operator=(Pipe &&o) noexcept
	{
		if (this != &o) {
			reset();
			fp_ = o.fp_;
			rc_ = o.rc_;
			o.fp_ = nullptr;
			o.rc_ = -1;
		}
		return *this;
	}

	/** popen 一个只读命令。失败返回 false */
	bool open(const char *cmd) noexcept
	{
		reset();
		fp_ = ::popen(cmd, "r");
		return fp_ != nullptr;
	}

	std::FILE *get() const noexcept { return fp_; }
	bool valid() const noexcept { return fp_ != nullptr; }

	/**
	 * 关闭并取退出码（等价于 waitpid 的 WEXITSTATUS）。
	 * 未打开时返回 -1。
	 */
	int close() noexcept
	{
		if (!fp_)
			return -1;

		int rc = ::pclose(fp_);

		fp_ = nullptr;
		if (rc != -1 && (rc & 0x7f) == 0)
			rc = (rc >> 8) & 0xff;      /* 取真正的 exit code */
		rc_ = rc;
		return rc;
	}

	void reset() noexcept { (void)close(); }
	int last_rc() const noexcept { return rc_; }

private:
	std::FILE *fp_ = nullptr;
	int rc_ = -1;
};

/* ------------------------------------------------------------------ */
/* mmap 缓冲区                                                        */
/* ------------------------------------------------------------------ */
/**
 * ★ 长度必须和指针一起存：munmap 要传"当初 mmap 的长度"，
 *   传错会留下映射或者踩掉别的映射，而且完全没有报错。
 */
class Mmap {
public:
	Mmap() noexcept = default;
	~Mmap() { reset(); }

	Mmap(const Mmap &) = delete;
	Mmap &operator=(const Mmap &) = delete;

	Mmap(Mmap &&o) noexcept : ptr_(o.ptr_), len_(o.len_)
	{
		o.ptr_ = nullptr;
		o.len_ = 0;
	}

	Mmap &operator=(Mmap &&o) noexcept
	{
		if (this != &o) {
			reset();
			ptr_ = o.ptr_;
			len_ = o.len_;
			o.ptr_ = nullptr;
			o.len_ = 0;
		}
		return *this;
	}

	bool map(int fd, std::size_t length, off_t offset,
		 int prot = PROT_READ | PROT_WRITE) noexcept
	{
		reset();
		void *p = ::mmap(nullptr, length, prot, MAP_SHARED, fd, offset);

		if (p == MAP_FAILED)
			return false;
		ptr_ = p;
		len_ = length;
		return true;
	}

	bool valid() const noexcept { return ptr_ != nullptr; }
	void *data() const noexcept { return ptr_; }
	std::size_t size() const noexcept { return len_; }

	template <typename T>
	T *as() const noexcept
	{
		return static_cast<T *>(ptr_);
	}

	void reset() noexcept
	{
		if (ptr_ && len_)
			::munmap(ptr_, len_);
		ptr_ = nullptr;
		len_ = 0;
	}

private:
	void *ptr_ = nullptr;
	std::size_t len_ = 0;
};

/* ------------------------------------------------------------------ */
/* getaddrinfo 结果                                                   */
/* ------------------------------------------------------------------ */
/**
 * getaddrinfo 的结果必须 freeaddrinfo 释放，否则每次调用漏一条链表。
 * 板子上 timesync 会周期性重试（断网时每 60 秒一次），漏几次不显眼，
 * 漏几百次就是几 MB —— 而这块板只有 54MB。
 */
class AddrInfo {
public:
	AddrInfo() noexcept = default;
	~AddrInfo() { reset(); }

	AddrInfo(const AddrInfo &) = delete;
	AddrInfo &operator=(const AddrInfo &) = delete;

	/**
	 * 解析失败返回 false（此时 *res 不会被赋值，内部保持空）。
	 * @param family AF_INET 或 AF_UNSPEC
	 * @param socktype SOCK_STREAM / SOCK_DGRAM，0 表示不限
	 */
	bool resolve(const char *host, const char *service, int family,
		     int socktype) noexcept
	{
		reset();

		addrinfo hints{};

		hints.ai_family = family;
		hints.ai_socktype = socktype;

		if (::getaddrinfo(host, service, &hints, &res_) != 0) {
			res_ = nullptr;
			return false;
		}
		return res_ != nullptr;
	}

	/** 第一个结果（通常够用：AF_INET + 明确 socktype 时只有一条） */
	const addrinfo *get() const noexcept { return res_; }
	bool valid() const noexcept { return res_ != nullptr; }
	explicit operator bool() const noexcept { return valid(); }

	void reset() noexcept
	{
		if (res_)
			::freeaddrinfo(res_);
		res_ = nullptr;
	}

private:
	addrinfo *res_ = nullptr;
};

/* ------------------------------------------------------------------ */
/* fork + execvp（绕开 shell）                                        */
/* ------------------------------------------------------------------ */
/**
 * 执行 argv 形式的命令并把输出读进 out。
 *
 * ★ 为什么不用 popen：popen 走 /bin/sh -c，命令字符串会被 shell
 *   解析一遍。要传给目标程序的引号/特殊字符会被吃掉。
 *   本项目实测：wpa_cli 下发 psk 时值必须带引号，走 shell 就变成
 *   裸字符串，wpa 按 64 位 hex 解析 → 报"密码长度需 8-63 位"
 *   （密码本身完全正确，报错极具误导）。
 *
 * @param argv  以 nullptr 结尾的参数数组，argv[0] 用绝对路径
 * @param out   输出缓冲，可为 nullptr
 * @param outsz out 的大小
 * @param merge_stderr true 则 stderr 也并入 out（输出量小的命令才用）
 * @return 子进程退出码；-1 表示 fork/pipe 失败
 */
inline int run_argv(const char *const argv[], char *out, std::size_t outsz,
		    bool merge_stderr = false)
{
	int pfd[2];

	if (!argv || !argv[0])
		return -1;
	if (out && outsz)
		out[0] = '\0';

	if (::pipe(pfd) != 0)
		return -1;

	/* pipe 的两端都用 RAII 管：任何一条 return 路径都不会漏 close */
	Fd rd(pfd[0]);
	Fd wr(pfd[1]);

	pid_t pid = ::fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		/* 子进程：写端接到 stdout，读端不需要 */
		::dup2(wr.get(), STDOUT_FILENO);
		/*
		 * merge_stderr：把 stderr 也接进同一根管子。
		 * 好处是"一次读完就能看到错误信息"，不用开两根管子再 select。
		 * 代价是 stdout/stderr 会交错，只适合输出量小的命令（wpa_cli 这种）。
		 */
		if (merge_stderr)
			::dup2(wr.get(), STDERR_FILENO);
		::close(rd.get());
		::close(wr.get());
		::execvp(argv[0], const_cast<char *const *>(argv));
		::_exit(127);                   /* exec 失败 */
	}

	/* 父进程：关掉写端，否则读永远等不到 EOF */
	wr.reset();

	if (out && outsz) {
		std::size_t used = 0;

		while (used + 1 < outsz) {
			ssize_t n = ::read(rd.get(), out + used, outsz - used - 1);

			if (n <= 0)
				break;
			used += static_cast<std::size_t>(n);
		}
		out[used] = '\0';
	} else {
		char sink[256];

		while (::read(rd.get(), sink, sizeof(sink)) > 0) {
			/* 丢弃 */
		}
	}
	rd.reset();

	int status = 0;

	if (::waitpid(pid, &status, 0) < 0)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -1;
}

/** popen 风格的便捷封装（走 shell，仅在确定命令安全时用） */
inline int run_shell(const char *cmd, char *out, std::size_t outsz)
{
	if (out && outsz)
		out[0] = '\0';

	Pipe p;

	if (!p.open(cmd))
		return -1;

	int rc;

	if (out && outsz) {
		std::size_t used = 0;

		while (used + 1 < outsz) {
			std::size_t n = std::fread(out + used, 1, outsz - used - 1, p.get());

			if (n == 0)
				break;
			used += n;
		}
		out[used] = '\0';
	}
	rc = p.close();
	return rc;
}

} /* namespace res */

#endif /* RESOURCES_HPP */
