/* Small single-client HTTP file exchange for the board's 54 MB RAM. */
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const size_t kMaxUpload = 8 * 1024 * 1024;
static const size_t kReserve = 4 * 1024 * 1024;
static const char *upload_dir;
static const char *camera_dir;
static std::atomic<int> active_clients{0};

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = static_cast<const char *>(data);
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static void reply(int fd, int code, const char *reason, const char *type,
                  const std::string &body)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     code, reason, type, body.size());
    send_all(fd, head, static_cast<size_t>(n));
    send_all(fd, body.data(), body.size());
}

static std::string escape_html(const char *s)
{
    std::string out;
    for (; *s; ++s) {
        switch (*s) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += *s;
        }
    }
    return out;
}

static std::string encode_url(const char *s)
{
    std::string out;
    char hex[4];
    for (; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            out += static_cast<char>(c);
        else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

static bool decode_name(const char *encoded, std::string &name)
{
    name.clear();
    for (size_t i = 0; encoded[i]; ++i) {
        unsigned char c = static_cast<unsigned char>(encoded[i]);
        if (c == '%') {
            unsigned int v;
            if (!encoded[i + 1] || !encoded[i + 2] ||
                !isxdigit(static_cast<unsigned char>(encoded[i + 1])) ||
                !isxdigit(static_cast<unsigned char>(encoded[i + 2])) ||
                sscanf(encoded + i + 1, "%2x", &v) != 1) return false;
            c = static_cast<unsigned char>(v);
            i += 2;
        }
        if (c < 32 || c == 127 || c == '/' || c == '\\') return false;
        name += static_cast<char>(c);
        if (name.size() > 120) return false;
    }
    return !name.empty() && name != "." && name != ".." && name[0] != '.';
}

static void append_files(std::string &html, const char *dir, const char *kind)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) && count < 100) {
        if (e->d_name[0] == '.') continue;
        std::string path = std::string(dir) + "/" + e->d_name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        html += "<li><a href=\"/download/";
        html += kind;
        html += "/" + encode_url(e->d_name) + "\" download>";
        html += escape_html(e->d_name);
        html += "</a> (" + std::to_string(static_cast<long long>(st.st_size)) + " B)</li>";
        ++count;
    }
    closedir(d);
}

static void page(int fd)
{
    struct statvfs fs{};
    unsigned long long free_mb = 0;
    if (statvfs(upload_dir, &fs) == 0)
        free_mb = static_cast<unsigned long long>(fs.f_bavail) * fs.f_frsize / 1048576;
    std::string html =
        "<!doctype html><html lang=zh><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>EchoMate 文件传输</title>"
        "<style>body{font:16px sans-serif;max-width:600px;margin:24px auto;padding:0 16px}"
        "button{padding:10px}li{margin:9px 0}a{overflow-wrap:anywhere}</style>"
        "<h2>EchoMate 文件传输</h2><p>上传到临时空间，重启会清空。"
        "单个文件最多 8 MB，并保留 4 MB 空间。</p><p>当前可用：" +
        std::to_string(free_mb) + " MB</p>"
        "<input id=f type=file multiple><button onclick=\"upload()\">上传</button>"
        "<p id=msg></p><h3>手机上传</h3><ul>";
    append_files(html, upload_dir, "upload");
    html += "</ul><h3>相机照片和视频</h3><ul>";
    append_files(html, camera_dir, "camera");
    html += "</ul><script>async function upload(){let m=document.getElementById('msg');"
            "for(let f of document.getElementById('f').files){m.textContent='上传 '+f.name;"
            "try{let r=await fetch('/upload/'+encodeURIComponent(f.name),"
            "{method:'PUT',body:f});if(!r.ok)throw Error(await r.text());}"
            "catch(e){m.textContent=f.name+'：'+e.message;return}}"
            "location.reload()}</script></html>";
    reply(fd, 200, "OK", "text/html; charset=utf-8", html);
}

static void download(int fd, const char *path)
{
    const char *slash = strchr(path, '/');
    if (!slash) { reply(fd, 400, "Bad Request", "text/plain", "bad path"); return; }
    std::string kind(path, slash - path), name;
    if (!decode_name(slash + 1, name) || (kind != "upload" && kind != "camera")) {
        reply(fd, 400, "Bad Request", "text/plain", "bad name"); return;
    }
    std::string file = std::string(kind == "upload" ? upload_dir : camera_dir) + "/" + name;
    int in = open(file.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (in < 0 || fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (in >= 0) close(in);
        reply(fd, 404, "Not Found", "text/plain", "not found"); return;
    }
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                       "Content-Disposition: attachment; filename=\"download\"; filename*=UTF-8''" +
                       encode_url(name.c_str()) + "\r\nContent-Length: " +
                       std::to_string(static_cast<long long>(st.st_size)) +
                       "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    if (send_all(fd, head.data(), head.size())) {
        char buf[8192];
        ssize_t got;
        while ((got = read(in, buf, sizeof(buf))) > 0)
            if (!send_all(fd, buf, static_cast<size_t>(got))) break;
    }
    close(in);
}

static void upload(int fd, const char *encoded, const std::string &raw, size_t body_at,
                   unsigned long long length)
{
    std::string name;
    if (!decode_name(encoded, name) || length > kMaxUpload) {
        reply(fd, 413, "Too Large", "text/plain", "invalid name or file over 8 MB"); return;
    }
    struct statvfs fs{};
    if (statvfs(upload_dir, &fs) != 0 ||
        static_cast<unsigned long long>(fs.f_bavail) * fs.f_frsize < length + kReserve) {
        reply(fd, 507, "Insufficient Storage", "text/plain", "not enough free space"); return;
    }
    std::string final = std::string(upload_dir) + "/" + name;
    struct stat st;
    if (lstat(final.c_str(), &st) == 0) {
        reply(fd, 409, "Conflict", "text/plain", "file already exists"); return;
    }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s/.incoming-XXXXXX", upload_dir);
    int out = mkstemp(tmp);
    if (out < 0) { reply(fd, 500, "Error", "text/plain", "cannot create file"); return; }
    unsigned long long left = length;
    size_t have = raw.size() - body_at;
    bool ok = have <= left;
    if (ok && have) {
        ok = write(out, raw.data() + body_at, have) == static_cast<ssize_t>(have);
        left -= have;
    }
    char buf[8192];
    while (ok && left) {
        ssize_t got = recv(fd, buf, left < sizeof(buf) ? static_cast<size_t>(left) : sizeof(buf), 0);
        if (got <= 0) { ok = false; break; }
        ok = write(out, buf, static_cast<size_t>(got)) == got;
        left -= static_cast<unsigned long long>(got);
    }
    if (close(out) != 0) ok = false;
    if (ok) ok = link(tmp, final.c_str()) == 0;
    unlink(tmp);
    if (ok) reply(fd, 201, "Created", "text/plain", "uploaded");
    else reply(fd, 500, "Error", "text/plain", "upload failed");
}

static void handle(int fd)
{
    struct timeval header_timeout{3, 0};
    struct timeval transfer_timeout{30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &header_timeout, sizeof(header_timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &transfer_timeout, sizeof(transfer_timeout));
    std::string raw;
    char buf[2048];
    size_t end;
    while ((end = raw.find("\r\n\r\n")) == std::string::npos && raw.size() < 8192) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        raw.append(buf, static_cast<size_t>(n));
    }
    if (end == std::string::npos) return;
    char method[16], path[2048];
    if (sscanf(raw.c_str(), "%15s %2047s", method, path) != 2) return;
    if (!strcmp(method, "GET") && !strcmp(path, "/")) { page(fd); return; }
    if (!strcmp(method, "GET") && !strncmp(path, "/download/", 10)) {
        download(fd, path + 10); return;
    }
    if (!strcmp(method, "PUT") && !strncmp(path, "/upload/", 8)) {
        unsigned long long length = 0;
        bool found = false;
        size_t p = raw.find("\r\n") + 2;
        while (p < end) {
            size_t next = raw.find("\r\n", p);
            if (next == std::string::npos || next > end) break;
            if (next - p >= 15 && !strncasecmp(raw.c_str() + p, "Content-Length:", 15)) {
                const char *v = raw.c_str() + p + 15;
                char *tail;
                length = strtoull(v, &tail, 10);
                found = tail != v && tail <= raw.c_str() + next;
            }
            p = next + 2;
        }
        if (!found) { reply(fd, 411, "Length Required", "text/plain", "missing length"); return; }
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &transfer_timeout, sizeof(transfer_timeout));
        upload(fd, path + 8, raw, end + 4, length);
        return;
    }
    reply(fd, 404, "Not Found", "text/plain", "not found");
}

static void *serve_client(void *arg)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    handle(fd);
    close(fd);
    active_clients.fetch_sub(1, std::memory_order_release);
    return nullptr;
}

int main(int argc, char **argv)
{
    const char *ip = argc > 1 ? argv[1] : "192.168.50.1";
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    upload_dir = getenv("TRANSFER_UPLOAD_DIR");
    camera_dir = getenv("TRANSFER_CAMERA_DIR");
    if (!upload_dir) upload_dir = "/tmp/phone_upload";
    if (!camera_dir) camera_dir = "/tmp/cam";
    mkdir(upload_dir, 0755);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(server, 4) != 0) {
        perror("transfer server"); return 1;
    }
    printf("file transfer listening at http://%s:%d\n", ip, port);
    fflush(stdout);
    for (;;) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) { if (errno == EINTR) continue; break; }
        if (active_clients.fetch_add(1, std::memory_order_acq_rel) >= 6) {
            active_clients.fetch_sub(1, std::memory_order_release);
            reply(client, 503, "Busy", "text/plain", "try again");
            close(client);
            continue;
        }
        pthread_attr_t attr;
        pthread_t thread;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 128 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&thread, &attr, serve_client,
                                reinterpret_cast<void *>(static_cast<intptr_t>(client)));
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            active_clients.fetch_sub(1, std::memory_order_release);
            close(client);
        }
    }
    return 0;
}
