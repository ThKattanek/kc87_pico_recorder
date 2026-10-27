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
//   kc87tool encode <datei.tap> [--out datei.csv]         TAP -> Pulsfolge (CSV), wie vom KC-87 geschrieben
//   kc87tool fromwav <datei.wav> [--out datei.csv]        WAV-Aufnahme -> Pulsfolge (CSV)
//   kc87tool play <port> <datei.csv|.tap|.wav>            Pulsfolge ueber den Pico an den KC-87 ausgeben

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
        const uint8_t* b = (const uint8_t*)p;
        while (len > 0) {       // blockiert, solange der Pico nichts annimmt (Flusskontrolle)
#ifdef _WIN32
            DWORD n = 0;
            if (!WriteFile(h_, b, (DWORD)len, &n, nullptr)) throw std::runtime_error("Schreibfehler");
#else
            ssize_t n = ::write(fd_, b, len);
            if (n < 0) throw std::runtime_error("Schreibfehler");
#endif
            b += n;
            len -= (size_t)n;
        }
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

// Kopfblock (Block 0) einer Maschinencode-Datei: Name (8), Typ (3), ab Byte 17 Anfangs-,
// End- und Startadresse (je 2 Bytes, little-endian). Das Layout ist aus ZM30 und einer
// eigenen SAVE-Aufnahme abgeleitet; Block 0 eines BASIC-Programms hat kein solches Format.
static std::string com_header(const Block& b) {
    if (b.nr != 0) return "";
    auto ok = [](uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!ok(b.data[0]) || !ok(b.data[8]) || !ok(b.data[9]) || !ok(b.data[10])) return "";
    std::string name;
    for (int i = 0; i < 8 && b.data[i]; ++i) name += (char)b.data[i];
    std::string typ((const char*)&b.data[8], 3);
    auto w = [&](int o) { return (unsigned)(b.data[o] | b.data[o + 1] << 8); };
    char buf[96];
    std::snprintf(buf, sizeof buf, "Datei \"%s.%s\"  Adressen %04X-%04X  Start %04X", name.c_str(), typ.c_str(),
                  w(17), w(19), w(21));
    return buf;
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
        std::string com = com_header(b);
        if (!com.empty()) std::printf("  %s", com.c_str());
        if (b.nr == 255) std::printf("  (letzter Block)");
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
// encode (TAP -> Pulsfolge) und play
// ---------------------------------------------------------------------------
// Pulsdauern wie in einer echten KC-87-Aufnahme gemessen (siehe Bandformat):
// jedes Symbol besteht aus zwei gleich langen Pulsen.
static constexpr double BIT0_US = 250.0;
static constexpr double BIT1_US = 455.0;
static constexpr double SEP_US = 870.0;
static constexpr int VORTON_FIRST = 6000;   // Perioden vor dem ersten Block
static constexpr int VORTON_NEXT = 160;     // Perioden vor jedem weiteren Block
static constexpr int VORTON_END = 160;      // Perioden nach dem letzten Block

static std::vector<Pulse> encode_tap(const std::string& tap) {
    std::ifstream f(tap, std::ios::binary);
    if (!f) throw std::runtime_error("Kann " + tap + " nicht lesen");
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    static const char HEAD[17] = "\xC3KC-TAPE by AF. ";
    if (d.size() < 16 || std::memcmp(d.data(), HEAD, 16) != 0)
        throw std::runtime_error(tap + " ist keine TAP-Datei (Kennung fehlt)");
    const size_t rec = 1 + BLOCK_DATA;
    if ((d.size() - 16) % rec != 0)
        std::printf("WARNUNG: TAP-Groesse passt nicht zu ganzen Bloecken, Rest wird ignoriert\n");

    std::vector<Pulse> p;
    auto period = [&](double us) { p.push_back({true, us}); p.push_back({false, us}); };
    size_t nblocks = (d.size() - 16) / rec;
    for (size_t b = 0; b < nblocks; ++b) {
        const uint8_t* r = &d[16 + b * rec];
        std::vector<uint8_t> bytes(r, r + rec);
        uint8_t sum = 0;
        for (size_t i = 1; i <= BLOCK_DATA; ++i) sum = (uint8_t)(sum + r[i]);
        bytes.push_back(sum);
        for (int i = 0; i < (b == 0 ? VORTON_FIRST : VORTON_NEXT); ++i) period(BIT1_US);
        for (uint8_t v : bytes) {
            period(SEP_US);
            for (int k = 0; k < 8; ++k) period((v >> k) & 1 ? BIT1_US : BIT0_US);
        }
        period(SEP_US);     // Trennzeichen nach dem letzten Byte des Blocks
    }
    // Nach dem letzten Block noch ein kurzer Vorton: der KC-87 schliesst den Block erst
    // mit der naechsten Flanke ab (auf dem echten Band folgt dort der naechste Vorton).
    for (int i = 0; i < VORTON_END; ++i) period(BIT1_US);
    std::printf("%zu Bloecke, %zu Pulse, Dauer %.1f s\n", nblocks, p.size(),
                [&] { double t = 0; for (auto& x : p) t += x.us; return t / 1e6; }());
    return p;
}

// ---------------------------------------------------------------------------
// WAV -> Pulsfolge
// ---------------------------------------------------------------------------
// Liest eine WAV-Datei (PCM 8/16/24/32 Bit oder Float32, erster Kanal) und macht daraus
// die Pulsfolge: die Nulldurchgaenge werden mit Hysterese (10 % der Spitze) gesucht und
// linear zwischen den Abtastwerten interpoliert, damit auch bei 44,1 kHz die Pulsdauern
// auf wenige us genau bleiben. Die Polaritaet spielt keine Rolle, da die Daten in den
// Pulsdauern stecken.
static std::vector<Pulse> pulses_from_wav(const std::string& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("Kann " + file + " nicht lesen");
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0)
        throw std::runtime_error(file + " ist keine WAV-Datei");

    uint32_t fmt = 0, ch = 0, rate = 0, bits = 0;
    size_t data_off = 0, data_len = 0;
    for (size_t o = 12; o + 8 <= d.size();) {
        size_t len = u32(o + 4);
        if (std::memcmp(&d[o], "fmt ", 4) == 0 && o + 24 <= d.size()) {
            fmt = u16(o + 8); ch = u16(o + 10); rate = u32(o + 12); bits = u16(o + 22);
            if (fmt == 0xFFFE && len >= 26 && o + 8 + 26 <= d.size()) fmt = u16(o + 8 + 24);   // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(&d[o], "data", 4) == 0) {
            data_off = o + 8;
            data_len = std::min(len, d.size() - data_off);
            break;
        }
        o += 8 + len + (len & 1);
    }
    if (!data_off || !ch || !rate) throw std::runtime_error("WAV-Kopf unvollstaendig");
    bool is_float = (fmt == 3 && bits == 32);
    if (!(fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !is_float)
        throw std::runtime_error("WAV-Format nicht unterstuetzt (PCM 8/16/24/32 Bit oder Float32)");

    size_t bps = bits / 8, frame = bps * ch, n = data_len / frame;
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* q = &d[data_off + i * frame];
        double v;
        if (is_float) { float t; std::memcpy(&t, q, 4); v = t; }
        else if (bits == 8) v = (q[0] - 128) / 128.0;
        else if (bits == 16) v = (int16_t)(q[0] | q[1] << 8) / 32768.0;
        else if (bits == 24) v = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) / 2147483648.0;
        else v = (int32_t)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24) / 2147483648.0;
        x[i] = (float)v;
    }
    if (n < 2) throw std::runtime_error("WAV enthaelt keine Daten");

    double mean = 0;
    for (float v : x) mean += v;
    mean /= (double)n;
    for (float& v : x) v = (float)(v - mean);
    std::vector<float> a(x.size());
    for (size_t i = 0; i < n; ++i) a[i] = std::fabs(x[i]);
    std::nth_element(a.begin(), a.begin() + (long)(n * 995 / 1000), a.end());
    double peak = a[n * 995 / 1000];
    if (peak < 1e-4) throw std::runtime_error("WAV ist praktisch stumm");
    double th = 0.1 * peak;

    std::vector<Pulse> p;
    int state = 0;                 // 0 = unbekannt, 1 = positiv, -1 = negativ
    double last_cross = 0, prev_edge = -1;
    bool prev_high = false;
    for (size_t i = 1; i < n; ++i) {
        if ((x[i - 1] < 0) != (x[i] < 0))
            last_cross = (double)(i - 1) + x[i - 1] / (double)(x[i - 1] - x[i]);   // interpolierter Nulldurchgang
        int ns = x[i] > th ? 1 : (x[i] < -th ? -1 : state);
        if (ns == state) continue;
        if (state != 0) {                       // echte Flanke (nicht der erste Pegel)
            if (prev_edge >= 0) p.push_back({prev_high, (last_cross - prev_edge) * 1e6 / rate});
            prev_edge = last_cross;
            prev_high = (ns == 1);
        }
        state = ns;
    }
    if (!p.empty() && !p.front().high) p.erase(p.begin());   // Folge beginnt mit einem High-Puls
    std::printf("WAV: %u Hz, %u Bit, %zu Kanaele, %.1f s, %zu Pulse\n", rate, bits, (size_t)ch,
                (double)n / rate, p.size());
    if (p.empty()) throw std::runtime_error("Keine Flanken in der WAV-Datei gefunden");
    return p;
}

static std::vector<Pulse> load_pulses(const std::string& file) {
    auto ends = [&](const char* e) {
        size_t n = std::strlen(e);
        return file.size() >= n && file.compare(file.size() - n, n, e) == 0;
    };
    if (ends(".tap") || ends(".TAP")) return encode_tap(file);
    if (ends(".wav") || ends(".WAV")) {
        auto p = pulses_from_wav(file);
        // Kurzer Vorton am Ende, damit der KC-87 den letzten Block mit einer Flanke abschliesst
        bool next_high = !p.back().high;
        for (int i = 0; i < 2 * VORTON_END; ++i, next_high = !next_high) p.push_back({next_high, BIT1_US});
        return p;
    }
    return load_csv(file);
}

static void play(const std::string& port, const std::string& file) {
    using clk = std::chrono::steady_clock;
    auto p = load_pulses(file);
    if (!p.empty() && !p.front().high) {
        std::printf("Hinweis: erster Puls ist Low, wird uebersprungen\n");
        p.erase(p.begin());
    }
    if (p.empty()) throw std::runtime_error("Keine Daten zum Abspielen");

    SerialPort sp(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint8_t tmp[4096];
    while (sp.read(tmp, sizeof tmp) > 0) {}      // Altes verwerfen

    sp.write("P", 1);
    uint8_t hdr[8];
    size_t got = 0;
    auto t0 = clk::now();
    while (got < 8 && clk::now() - t0 < std::chrono::seconds(3)) got += sp.read(hdr + got, 8 - got);
    std::vector<uint8_t> h(hdr, hdr + got);
    if (got < 8 || rd32(h, 0) != MAGIC)
        throw std::runtime_error("Kein gueltiger Header - laeuft die Firmware mit Wiedergabe auf dem Pico?");
    double tick_hz = rd32(h, 4);

    double total_us = 0;
    for (auto& x : p) total_us += x.us;
    std::printf("Wiedergabe: %zu Pulse, ca. %.1f s - jetzt am KC-87 den Ladevorgang starten (CLOAD / LOAD)\n",
                p.size(), total_us / 1e6);

    std::vector<uint8_t> buf;
    buf.reserve(p.size() * 4 + 4);
    for (auto& x : p) {
        uint32_t t = (uint32_t)std::llround(x.us * tick_hz / 1e6);
        if (t >= END_MARKER) t = END_MARKER - 1;
        for (int k = 0; k < 4; ++k) buf.push_back((uint8_t)(t >> (8 * k)));
    }
    for (int k = 0; k < 4; ++k) buf.push_back(0xFF);

    const size_t CHUNK = 1024;
    auto next_info = clk::now() + std::chrono::seconds(1);
    for (size_t off = 0; off < buf.size(); off += CHUNK) {
        sp.write(&buf[off], std::min(CHUNK, buf.size() - off));
        if (clk::now() >= next_info) {
            std::printf("  gesendet %3d %%\r", (int)(100.0 * off / buf.size()));
            std::fflush(stdout);
            next_info += std::chrono::seconds(1);
        }
    }
    std::printf("  gesendet 100 %%, warte auf Ende der Ausgabe ...\n");

    std::vector<uint8_t> tr;
    auto give_up = clk::now() + std::chrono::seconds(30);
    while (tr.size() < 12 && clk::now() < give_up) {
        size_t n = sp.read(tmp, sizeof tmp);
        tr.insert(tr.end(), tmp, tmp + n);
    }
    if (tr.size() < 12 || rd32(tr, 0) != END_MARKER)
        throw std::runtime_error("Keine Abschlussmeldung vom Pico empfangen");
    uint32_t status = rd32(tr, 4), count = rd32(tr, 8);
    if (status & 1u) std::printf("WARNUNG: Pico-Puffer lief leer (Unterlauf) - Zeiten gestoert\n");
    if (status & 2u) std::printf("WARNUNG: Wiedergabe abgebrochen (Timeout oder USB getrennt)\n");
    std::printf("Fertig: %u Pulse ausgegeben\n", count);
}

// ---------------------------------------------------------------------------
static void usage() {
    std::cout << "Benutzung:\n"
                 "  kc87tool record <port> [--sec 30] [--out aufnahme]\n"
                 "  kc87tool analyze <datei.csv>\n"
                 "  kc87tool decode <datei.csv> [--tap datei.tap]\n"
                 "  kc87tool wav <datei.csv> [--rate 44100]\n"
                 "  kc87tool encode <datei.tap> [--out datei.csv]\n"
                 "  kc87tool fromwav <datei.wav> [--out datei.csv]\n"
                 "  kc87tool play <port> <datei.csv|datei.tap|datei.wav>\n";
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) { usage(); return 1; }
        std::string cmd = argv[1], arg = argv[2];
        std::map<std::string, std::string> opt;
        int first_opt = (cmd == "play") ? 4 : 3;
        for (int i = first_opt; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
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
        } else if (cmd == "encode") {
            auto p = encode_tap(arg);
            std::string out = get("--out", (base_name(arg) + ".csv").c_str());
            save_csv(out, p);
            std::printf("Pulsfolge geschrieben: %s\n", out.c_str());
        } else if (cmd == "fromwav") {
            auto p = pulses_from_wav(arg);
            std::string out = get("--out", (base_name(arg) + ".csv").c_str());
            save_csv(out, p);
            std::printf("Pulsfolge geschrieben: %s\n", out.c_str());
        } else if (cmd == "play") {
            if (argc < 4) { usage(); return 1; }
            play(arg, argv[3]);
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
