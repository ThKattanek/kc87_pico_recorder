// kc87tool - KC-87 Signal vom Pico aufzeichnen und auswerten (C++17, Windows/Linux/macOS)
//
// Bauen:
//   CMake       : cmake -S . -B build && cmake --build build           (alle Systeme)
//                 Ergebnis: build/kc87tool (Windows/Visual Studio: build/Release/kc87tool.exe,
//                 dort mit: cmake --build build --config Release)
//   Linux/macOS : g++ -std=c++17 -O2 -o kc87tool kc87tool.cpp
//   Windows     : g++ -std=c++17 -O2 -o kc87tool.exe kc87tool.cpp      (MinGW/MSYS2)
//                 cl /std:c++17 /O2 /EHsc kc87tool.cpp                  (Visual Studio)
//
// Benutzung:
//   kc87tool record <port> [--sec 30] [--out aufnahme]   z.B. COM5 oder /dev/ttyACM0
//   kc87tool analyze <datei.csv>                          Statistik
//   kc87tool decode <datei.csv> [--tap datei.tap]         Bloecke + Programmname, optional TAP schreiben
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
}

// ---------------------------------------------------------------------------
// decode
// ---------------------------------------------------------------------------
// Bandformat (aus einer echten CSAVE-Aufnahme abgelesen):
//   Eine Periode besteht aus zwei gleich langen Pulsen (High + Low):
//     Bit 0      : 2 x ~250 us  (~2 kHz)
//     Bit 1      : 2 x ~455 us  (~1,1 kHz)
//     Trennzeichen: 2 x ~870 us (~575 Hz)
//   Ein Byte sind 8 Bits (LSB zuerst), jedem Byte geht ein Trennzeichen voraus.
//   Vor jedem Block steht ein Vorton aus 1-Bits (kein gueltiges Byte).
//   Block = 130 Bytes: Blocknummer, 128 Datenbytes, Pruefsumme (Summe der
//   Datenbytes modulo 256).
//   BASIC-Programme (CSAVE) enthalten D3 D3 D3 und danach den Programmnamen
//   mit 8 Zeichen.
static constexpr double SHORT_MAX_US = 350.0;  // darunter: Bit 0
static constexpr double MID_MAX_US = 650.0;    // darunter: Bit 1, darueber: Trennzeichen
static constexpr size_t BLOCK_DATA = 128;

struct Block {
    uint8_t nr;
    uint8_t data[BLOCK_DATA];
    bool checksum_ok;
};

// Liefert die Bytes in Abschnitten; ein Abschnitt endet, sobald statt eines
// Bytes (8 Bits) etwas anderes folgt, z.B. der Vorton vor dem naechsten Block.
static std::vector<std::vector<uint8_t>> decode_segments(const std::vector<Pulse>& p, size_t& bad_periods) {
    auto cls = [](double us) { return us < SHORT_MAX_US ? 'S' : (us < MID_MAX_US ? 'M' : 'L'); };
    std::vector<std::vector<uint8_t>> segs(1);
    std::vector<int> bits;
    bool in_byte = false;
    bad_periods = 0;

    auto end_group = [&]() {
        if (!in_byte) return;
        if (bits.size() == 8) {
            uint8_t v = 0;
            for (int k = 0; k < 8; ++k) v |= (uint8_t)(bits[k] << k);
            segs.back().push_back(v);
        } else if (!segs.back().empty()) {
            segs.emplace_back();
        }
        bits.clear();
    };

    for (size_t i = 0; i + 1 < p.size();) {
        char a = cls(p[i].us), b = cls(p[i + 1].us);
        if (a != b) { ++bad_periods; ++i; continue; }   // Pulse nicht paarweise gleich: neu synchronisieren
        i += 2;
        if (a == 'L') {
            end_group();
            in_byte = true;
        } else if (in_byte) {
            bits.push_back(a == 'M' ? 1 : 0);
        }
    }
    end_group();
    if (segs.back().empty()) segs.pop_back();
    return segs;
}

static std::vector<Block> blocks_from_segments(const std::vector<std::vector<uint8_t>>& segs) {
    std::vector<Block> blocks;
    for (const auto& s : segs) {
        if (s.size() != 1 + BLOCK_DATA + 1) continue;
        Block b{};
        b.nr = s[0];
        uint8_t sum = 0;
        for (size_t i = 0; i < BLOCK_DATA; ++i) { b.data[i] = s[1 + i]; sum = (uint8_t)(sum + b.data[i]); }
        b.checksum_ok = (sum == s[1 + BLOCK_DATA]);
        blocks.push_back(b);
    }
    return blocks;
}

static std::string basic_name(const uint8_t* d, size_t len) {
    for (size_t i = 0; i + 11 <= len; ++i) {
        if (d[i] != 0xD3 || d[i + 1] != 0xD3 || d[i + 2] != 0xD3) continue;
        std::string name;
        for (size_t k = 0; k < 8; ++k) {
            uint8_t c = d[i + 3 + k];
            name += (c >= 32 && c < 127) ? (char)c : '?';
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        return name;
    }
    return "";
}

static std::vector<Block> decode(const std::string& csv) {
    auto p = load_csv(csv);
    if (p.empty()) throw std::runtime_error("Keine Daten in " + csv);
    size_t bad = 0;
    auto segs = decode_segments(p, bad);
    auto blocks = blocks_from_segments(segs);
    size_t nbytes = 0;
    for (const auto& s : segs) nbytes += s.size();
    std::printf("Dekodiert: %zu Bytes, %zu Bloecke", nbytes, blocks.size());
    if (bad) std::printf(" (%zu ungueltige Perioden uebersprungen)", bad);
    std::printf("\n");

    for (const auto& b : blocks) {
        std::string name = basic_name(b.data, BLOCK_DATA);
        std::printf("  Block %3u: Pruefsumme %s", b.nr, b.checksum_ok ? "OK" : "FALSCH");
        if (!name.empty()) std::printf("  BASIC-Programm \"%s\"", name.c_str());
        std::printf("\n");
    }
    if (blocks.empty()) {
        std::printf("Keine vollstaendigen Bloecke gefunden. Erste Bytes:\n ");
        const auto& s = segs.front();
        for (size_t i = 0; i < s.size() && i < 48; ++i) std::printf(" %02X", s[i]);
        std::printf("\n");
    }
    return blocks;
}

// TAP-Datei: 16 Byte Kennung, danach je Block Blocknummer + 128 Datenbytes.
static void write_tap(const std::string& file, const std::vector<Block>& blocks) {
    static const char HEAD[17] = "\xC3KC-TAPE by AF. ";
    std::ofstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("Kann " + file + " nicht schreiben");
    f.write(HEAD, 16);
    size_t n = 0;
    for (const auto& b : blocks) {
        if (!b.checksum_ok) { std::printf("  Block %u mit falscher Pruefsumme uebersprungen\n", b.nr); continue; }
        f.put((char)b.nr);
        f.write((const char*)b.data, BLOCK_DATA);
        ++n;
    }
    std::printf("TAP geschrieben: %s (%zu Bloecke)\n", file.c_str(), n);
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
                 "  kc87tool decode <datei.csv> [--tap datei.tap]\n"
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
            write_tap(base_name(csv) + ".tap", decode(csv));
        } else if (cmd == "analyze") {
            analyze(arg);
        } else if (cmd == "decode") {
            auto blocks = decode(arg);
            if (opt.count("--tap")) write_tap(opt["--tap"], blocks);
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
