// kc87tool - KC-87 Signal vom Pico aufzeichnen und auswerten (C++17, Windows/Linux/macOS)
//
// Bauen:
//   Linux/macOS : g++ -std=c++17 -O2 -o kc87tool kc87tool.cpp
//   Windows     : g++ -std=c++17 -O2 -o kc87tool.exe kc87tool.cpp      (MinGW/MSYS2)
//                 cl /std:c++17 /O2 /EHsc kc87tool.cpp                  (Visual Studio)
//
// Benutzung:
//   kc87tool record <port> [--sec 30] [--out aufnahme]   z.B. COM5 oder /dev/ttyACM0
//   kc87tool analyze <datei.csv>                          Statistik + SVG-Diagramme
//   kc87tool wav <datei.csv> [--rate 44100]               Rechtecksignal als WAV

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

static constexpr uint32_t MAGIC = 0x3738434Bu;  // "KC87"
static constexpr uint32_t END_MARKER = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Serielle Schnittstelle
// ---------------------------------------------------------------------------
class SerialPort {
public:
    explicit SerialPort(const std::string& name) {
#ifdef _WIN32
        std::string path = name.rfind("\\\\.\\", 0) == 0 ? name : "\\\\.\\" + name;
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Kann " + name + " nicht oeffnen");
        DCB dcb{};
        dcb.DCBlength = sizeof dcb;
        GetCommState(h_, &dcb);
        dcb.BaudRate = 115200;          // bei USB-CDC egal, aber NICHT 1200 (Reset in BOOTSEL)
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;   // Pico sendet nur bei gesetztem DTR
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = dcb.fInX = FALSE;
        SetCommState(h_, &dcb);
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = 20;
        to.ReadTotalTimeoutConstant = 100;
        to.WriteTotalTimeoutConstant = 1000;
        SetCommTimeouts(h_, &to);
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        fd_ = ::open(name.c_str(), O_RDWR | O_NOCTTY);
        if (fd_ < 0) throw std::runtime_error("Kann " + name + " nicht oeffnen");
        termios t{};
        tcgetattr(fd_, &t);
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 1;              // 100 ms
        tcsetattr(fd_, TCSANOW, &t);
        int dtr = TIOCM_DTR;
        ioctl(fd_, TIOCMBIS, &dtr);
        tcflush(fd_, TCIOFLUSH);
#endif
    }
    ~SerialPort() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    size_t read(uint8_t* buf, size_t len) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(h_, buf, (DWORD)len, &n, nullptr)) throw std::runtime_error("Lesefehler");
        return n;
#else
        ssize_t n = ::read(fd_, buf, len);
        if (n < 0) throw std::runtime_error("Lesefehler");
        return (size_t)n;
#endif
    }
    void write(const void* p, size_t len) {
#ifdef _WIN32
        DWORD n = 0;
        WriteFile(h_, p, (DWORD)len, &n, nullptr);
#else
        if (::write(fd_, p, len) < 0) throw std::runtime_error("Schreibfehler");
#endif
    }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// ---------------------------------------------------------------------------
// Daten
// ---------------------------------------------------------------------------
struct Pulse {
    bool high;
    double us;
};

static uint32_t rd32(const std::vector<uint8_t>& d, size_t off) {
    return (uint32_t)d[off] | (uint32_t)d[off + 1] << 8 | (uint32_t)d[off + 2] << 16 |
           (uint32_t)d[off + 3] << 24;
}

static std::string base_name(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t sep = path.find_last_of("/\\");
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) return path.substr(0, dot);
    return path;
}

static void save_csv(const std::string& file, const std::vector<Pulse>& p) {
    std::ofstream f(file);
    if (!f) throw std::runtime_error("Kann " + file + " nicht schreiben");
    f << "level;dauer_us\n";
    char line[64];
    for (const auto& x : p) {
        std::snprintf(line, sizeof line, "%d;%.3f\n", x.high ? 1 : 0, x.us);
        f << line;
    }
}

static std::vector<Pulse> load_csv(const std::string& file) {
    std::ifstream f(file);
    if (!f) throw std::runtime_error("Kann " + file + " nicht lesen");
    std::vector<Pulse> p;
    std::string line;
    std::getline(f, line);  // Kopfzeile
    while (std::getline(f, line)) {
        size_t s = line.find(';');
        if (s == std::string::npos) continue;
        p.push_back({std::stoi(line.substr(0, s)) != 0, std::stod(line.substr(s + 1))});
    }
    return p;
}

// ---------------------------------------------------------------------------
// record
// ---------------------------------------------------------------------------
static std::string record(const std::string& port, double sec, const std::string& out) {
    using clk = std::chrono::steady_clock;
    SerialPort sp(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint8_t tmp[65536];
    while (sp.read(tmp, sizeof tmp) > 0) {}      // Altes verwerfen

    sp.write("S", 1);
    std::cout << "Aufnahme laeuft " << sec << " s ... (jetzt Signal am KC-87 starten)\n";

    std::vector<uint8_t> data;
    data.reserve(8 << 20);
    auto t0 = clk::now();
    auto t_stop = t0 + std::chrono::milliseconds((long long)(sec * 1000));
    auto next_info = t0 + std::chrono::seconds(1);
    bool stop_sent = false;
    size_t scan = 8;          // naechstes zu pruefendes Wort (nach dem Header)
    long end_pos = -1;
    clk::time_point give_up{};

    while (true) {
        size_t n = sp.read(tmp, sizeof tmp);
        data.insert(data.end(), tmp, tmp + n);
        auto now = clk::now();

        if (!stop_sent && now >= t_stop) {
            sp.write("X", 1);
            stop_sent = true;
            give_up = now + std::chrono::seconds(3);
        }
        if (!stop_sent && now >= next_info) {
            std::cout << "  " << (data.size() > 8 ? (data.size() - 8) / 4 : 0) << " Pulse\r" << std::flush;
            next_info += std::chrono::seconds(1);
        }
        if (stop_sent) {
            for (; scan + 4 <= data.size(); scan += 4)
                if (rd32(data, scan) == END_MARKER) { end_pos = (long)scan; break; }
            if (end_pos >= 0 && data.size() >= (size_t)end_pos + 12) break;
            if (now >= give_up) break;
        }
    }
    std::cout << "\n";

    if (data.size() < 8 || rd32(data, 0) != MAGIC)
        throw std::runtime_error("Kein gueltiger Header empfangen - laeuft die Firmware auf dem Pico?");
    double tick_hz = rd32(data, 4);
    if (end_pos < 0) {
        std::cout << "WARNUNG: kein Endmarker empfangen, Daten evtl. unvollstaendig\n";
        end_pos = (long)((data.size() - 8) / 4 * 4 + 8);
    } else {
        uint32_t status = rd32(data, end_pos + 4);
        uint32_t count = rd32(data, end_pos + 8);
        if (status & 1u) std::cout << "WARNUNG: Ringpuffer-Ueberlauf im Pico - Pulse verloren\n";
        if (status & 2u) std::cout << "WARNUNG: PIO-Stall - Zeiten unzuverlaessig\n";
        if (count != (uint32_t)((end_pos - 8) / 4))
            std::cout << "WARNUNG: " << count << " Werte gesendet, " << (end_pos - 8) / 4 << " empfangen\n";
    }

    std::vector<Pulse> pulses;
    pulses.reserve((end_pos - 8) / 4);
    for (size_t off = 8, i = 0; off < (size_t)end_pos; off += 4, ++i)
        pulses.push_back({i % 2 == 0, rd32(data, off) * 1e6 / tick_hz});

    std::string csv = out + ".csv";
    save_csv(csv, pulses);
    std::cout << pulses.size() << " Pulse gespeichert in " << csv
              << "  (Aufloesung " << 1e6 / tick_hz << " us)\n";
    return csv;
}

// ---------------------------------------------------------------------------
// analyze
// ---------------------------------------------------------------------------
static void stats(const char* name, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    std::printf("%-8s min %9.1f us  median %9.1f us  max %11.1f us\n", name, v.front(),
                v[v.size() / 2], v.back());
}

static void write_hist_svg(const std::string& file, const std::vector<int>& hist, double bin_us) {
    const int W = 1000, H = 420, L = 60, R = 20, T = 30, B = 50;
    int maxc = std::max(1, *std::max_element(hist.begin(), hist.end()));
    double lmax = std::log10(maxc + 1.0);
    std::ofstream f(file);
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
      << "' font-family='sans-serif' font-size='12'>\n<rect width='100%' height='100%' fill='white'/>\n"
      << "<text x='" << W / 2 << "' y='18' text-anchor='middle' font-size='14'>"
         "Histogramm der Periodendauern (High+Low), log. Skala</text>\n";
    double bw = double(W - L - R) / hist.size();
    for (size_t i = 0; i < hist.size(); ++i) {
        if (!hist[i]) continue;
        double h = std::log10(hist[i] + 1.0) / lmax * (H - T - B);
        f << "<rect x='" << L + i * bw << "' y='" << H - B - h << "' width='" << std::max(bw, 1.0)
          << "' height='" << h << "' fill='#3b6fb6'/>\n";
    }
    f << "<line x1='" << L << "' y1='" << H - B << "' x2='" << W - R << "' y2='" << H - B
      << "' stroke='black'/>\n";
    double range = hist.size() * bin_us;
    for (double t = 0; t <= range + 1e-9; t += 500) {
        double x = L + t / range * (W - L - R);
        f << "<line x1='" << x << "' y1='" << H - B << "' x2='" << x << "' y2='" << H - B + 5
          << "' stroke='black'/><text x='" << x << "' y='" << H - B + 20
          << "' text-anchor='middle'>" << (int)t << "</text>\n";
    }
    f << "<text x='" << W / 2 << "' y='" << H - 10 << "' text-anchor='middle'>Periodendauer [us]</text>\n"
      << "<text x='15' y='" << H / 2 << "' transform='rotate(-90 15 " << H / 2
      << ")' text-anchor='middle'>Anzahl (max " << maxc << ")</text>\n</svg>\n";
}

static void write_wave_svg(const std::string& file, const std::vector<Pulse>& p, size_t m) {
    const int W = 1000, H = 200, L = 40, R = 20, T = 30, B = 40;
    double total = 0;
    for (size_t i = 0; i < m; ++i) total += p[i].us;
    if (total <= 0) return;
    std::ofstream f(file);
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
      << "' font-family='sans-serif' font-size='12'>\n<rect width='100%' height='100%' fill='white'/>\n"
      << "<text x='" << W / 2 << "' y='18' text-anchor='middle' font-size='14'>Erste " << m
      << " Pulse (" << total / 1000.0 << " ms)</text>\n<polyline fill='none' stroke='#3b6fb6' points='";
    double yh = T, yl = H - B, t = 0;
    for (size_t i = 0; i < m; ++i) {
        double y = p[i].high ? yh : yl;
        double x0 = L + t / total * (W - L - R);
        t += p[i].us;
        double x1 = L + t / total * (W - L - R);
        f << x0 << ',' << y << ' ' << x1 << ',' << y << ' ';
    }
    f << "'/>\n<text x='" << W / 2 << "' y='" << H - 10 << "' text-anchor='middle'>Zeit</text>\n</svg>\n";
}

static void analyze(const std::string& csv) {
    auto p = load_csv(csv);
    if (p.empty()) throw std::runtime_error("Keine Daten in " + csv);
    std::vector<double> hi, lo, per;
    double total = 0;
    for (auto& x : p) {
        (x.high ? hi : lo).push_back(x.us);
        total += x.us;
    }
    for (size_t i = 0; i + 1 < p.size(); i += 2) per.push_back(p[i].us + p[i + 1].us);

    std::printf("Pulse: %zu, Gesamtdauer: %.2f s\n", p.size(), total / 1e6);
    stats("High", hi);
    stats("Low", lo);
    stats("Periode", per);

    const double BIN = 20.0, MAXP = 5000.0;
    std::vector<int> hist((size_t)(MAXP / BIN), 0);
    for (double v : per)
        if (v < MAXP) hist[(size_t)(v / BIN)]++;

    std::vector<size_t> idx(hist.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return hist[a] > hist[b]; });
    idx.resize(std::min<size_t>(6, idx.size()));
    std::sort(idx.begin(), idx.end());
    std::printf("Haeufigste Periodenlaengen (20-us-Klassen):\n");
    for (size_t i : idx)
        if (hist[i])
            std::printf("  %5.0f-%5.0f us: %7dx  (~%.0f Hz)\n", i * BIN, (i + 1) * BIN, hist[i],
                        1e6 / (i * BIN + BIN / 2));

    std::string b = base_name(csv);
    write_hist_svg(b + "_histogramm.svg", hist, BIN);
    write_wave_svg(b + "_verlauf.svg", p, std::min<size_t>(400, p.size()));
    std::printf("Diagramme: %s_histogramm.svg, %s_verlauf.svg (im Browser oeffnen)\n", b.c_str(), b.c_str());
}

// ---------------------------------------------------------------------------
// wav
// ---------------------------------------------------------------------------
static void put16(std::ofstream& f, uint16_t v) { f.put(char(v & 0xFF)).put(char(v >> 8)); }
static void put32(std::ofstream& f, uint32_t v) { put16(f, uint16_t(v)); put16(f, uint16_t(v >> 16)); }

static void to_wav(const std::string& csv, uint32_t rate) {
    auto p = load_csv(csv);
    std::vector<uint8_t> s;
    double t = 0;
    uint64_t pos = 0;
    for (auto& x : p) {
        t += x.us;
        uint64_t edge = (uint64_t)std::llround(t * rate / 1e6);
        s.insert(s.end(), edge - pos, x.high ? 216 : 40);
        pos = edge;
    }
    std::string out = base_name(csv) + ".wav";
    std::ofstream f(out, std::ios::binary);
    f.write("RIFF", 4); put32(f, 36 + (uint32_t)s.size()); f.write("WAVE", 4);
    f.write("fmt ", 4); put32(f, 16); put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate);
    put16(f, 1); put16(f, 8);
    f.write("data", 4); put32(f, (uint32_t)s.size());
    f.write((const char*)s.data(), (std::streamsize)s.size());
    std::printf("WAV geschrieben: %s (%.1f s)\n", out.c_str(), double(s.size()) / rate);
}

// ---------------------------------------------------------------------------
static void usage() {
    std::cout << "Benutzung:\n"
                 "  kc87tool record <port> [--sec 30] [--out aufnahme]\n"
                 "  kc87tool analyze <datei.csv>\n"
                 "  kc87tool wav <datei.csv> [--rate 44100]\n";
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) { usage(); return 1; }
        std::string cmd = argv[1], arg = argv[2];
        std::map<std::string, std::string> opt;
        for (int i = 3; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
        auto get = [&](const char* k, const char* d) { return opt.count(k) ? opt[k] : std::string(d); };

        if (cmd == "record") {
            std::string csv = record(arg, std::stod(get("--sec", "30")), get("--out", "aufnahme"));
            analyze(csv);
        } else if (cmd == "analyze") {
            analyze(arg);
        } else if (cmd == "wav") {
            to_wav(arg, (uint32_t)std::stoul(get("--rate", "44100")));
        } else {
            usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Fehler: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
