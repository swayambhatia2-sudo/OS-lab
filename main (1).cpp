/*
 * ============================================================
 *  FULL-STACK C++ WEB APPLICATION
 *  Backend  : Raw POSIX socket HTTP/1.1 server
 *  Frontend : Embedded HTML/CSS/JS (black & white theme)
 *  Features : REST API · Notes CRUD · Live Stats · JSON
 *  Build    : g++ -std=c++17 -O2 -lpthread -o server main.cpp
 *  Run      : ./server 8080
 *  Visit    : http://localhost:8080
 * ============================================================
 */

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <csignal>

// POSIX socket headers
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ─── ANSI COLORS FOR TERMINAL LOGS ───────────────────────────────────────────
#define RST  "\033[0m"
#define BLD  "\033[1m"
#define GRN  "\033[32m"
#define YLW  "\033[33m"
#define BLU  "\033[34m"
#define CYN  "\033[36m"
#define RED  "\033[31m"
#define DIM  "\033[2m"

// ─── DATA MODELS ─────────────────────────────────────────────────────────────
struct Note {
    int         id;
    std::string title;
    std::string body;
    std::string tag;
    std::string created_at;
    std::string color;   // "white" | "accent"
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> params;
};

struct HttpResponse {
    int         status  = 200;
    std::string content_type = "application/json";
    std::string body;
    std::map<std::string, std::string> headers;
};

// ─── IN-MEMORY DATABASE ──────────────────────────────────────────────────────
std::mutex              db_mutex;
std::vector<Note>       notes_db;
std::atomic<int>        id_counter{1};
std::atomic<long long>  request_count{0};
std::atomic<long long>  bytes_served{0};
std::chrono::steady_clock::time_point server_start;

std::string current_time_str() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::string s(std::ctime(&t));
    s.erase(s.length()-1); // remove newline
    return s;
}

std::string uptime_str() {
    auto now  = std::chrono::steady_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - server_start).count();
    long h = secs/3600, m = (secs%3600)/60, s = secs%60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02ldh %02ldm %02lds", h, m, s);
    return std::string(buf);
}

// ─── JSON HELPERS ─────────────────────────────────────────────────────────────
std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

std::string note_to_json(const Note& n) {
    std::ostringstream o;
    o << "{"
      << "\"id\":"         << n.id            << ","
      << "\"title\":\""    << json_escape(n.title)      << "\","
      << "\"body\":\""     << json_escape(n.body)       << "\","
      << "\"tag\":\""      << json_escape(n.tag)        << "\","
      << "\"color\":\""    << json_escape(n.color)      << "\","
      << "\"created_at\":\"" << json_escape(n.created_at) << "\""
      << "}";
    return o.str();
}

std::string notes_array_json(const std::vector<Note>& ns) {
    std::string out = "[";
    for (size_t i = 0; i < ns.size(); ++i) {
        out += note_to_json(ns[i]);
        if (i + 1 < ns.size()) out += ",";
    }
    out += "]";
    return out;
}

// Simple JSON value extractor (no external deps)
std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < json.size() && (json[pos]==' '||json[pos]=='\t')) ++pos;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos]=='\\' && pos+1<json.size()) {
                char c = json[pos+1];
                if (c=='n') val+='\n';
                else if (c=='t') val+='\t';
                else val+=c;
                pos+=2;
            } else {
                val += json[pos++];
            }
        }
        return val;
    }
    // number / bool / null
    size_t end = pos;
    while (end < json.size() && json[end]!=',' && json[end]!='}' && json[end]!=']') ++end;
    return json.substr(pos, end-pos);
}

// ─── HTTP PARSER ─────────────────────────────────────────────────────────────
HttpRequest parse_request(const std::string& raw) {
    HttpRequest req;
    std::istringstream ss(raw);
    std::string line;
    // Request line
    std::getline(ss, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    std::istringstream rl(line);
    std::string full_path;
    rl >> req.method >> full_path;
    size_t q = full_path.find('?');
    if (q != std::string::npos) {
        req.path  = full_path.substr(0, q);
        req.query = full_path.substr(q+1);
    } else {
        req.path = full_path;
    }
    // Headers
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (line.empty()) break;
        size_t col = line.find(':');
        if (col != std::string::npos) {
            std::string k = line.substr(0,col);
            std::string v = line.substr(col+1);
            while (!v.empty() && v.front()==' ') v.erase(v.begin());
            req.headers[k] = v;
        }
    }
    // Body (rest of stream)
    std::string remainder((std::istreambuf_iterator<char>(ss)), {});
    req.body = remainder;
    return req;
}

std::string build_response(const HttpResponse& res) {
    std::map<int,std::string> STATUS_MSG = {
        {200,"OK"},{201,"Created"},{204,"No Content"},
        {400,"Bad Request"},{404,"Not Found"},{405,"Method Not Allowed"},
        {500,"Internal Server Error"}
    };
    std::ostringstream out;
    out << "HTTP/1.1 " << res.status << " " << STATUS_MSG[res.status] << "\r\n";
    out << "Content-Type: "   << res.content_type << "\r\n";
    out << "Content-Length: " << res.body.size()  << "\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: Content-Type\r\n";
    out << "Connection: close\r\n";
    for (auto& [k,v] : res.headers) out << k << ": " << v << "\r\n";
    out << "\r\n";
    out << res.body;
    return out.str();
}

// ─── EMBEDDED FRONTEND ────────────────────────────────────────────────────────
const std::string FRONTEND_HTML = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>NoteOS — C++ Full-Stack</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=IM+Fell+English:ital@0;1&display=swap');

  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  :root {
    --bg:      #000000;
    --bg2:     #0a0a0a;
    --bg3:     #111111;
    --border:  #2a2a2a;
    --border2: #404040;
    --text:    #ffffff;
    --text2:   #cccccc;
    --dim:     #666666;
    --accent:  #ffffff;
    --font:    'Times New Roman', 'IM Fell English', Georgia, serif;
    --mono:    'Courier New', Courier, monospace;
  }

  html, body {
    background: var(--bg);
    color: var(--text);
    font-family: var(--font);
    min-height: 100vh;
    scroll-behavior: smooth;
  }

  /* ── SCROLLBAR ── */
  ::-webkit-scrollbar { width: 6px; }
  ::-webkit-scrollbar-track { background: var(--bg); }
  ::-webkit-scrollbar-thumb { background: var(--border2); }

  /* ── HEADER / NAV ── */
  header {
    position: sticky; top: 0; z-index: 100;
    background: var(--bg);
    border-bottom: 1px solid var(--border);
    display: flex; align-items: center; justify-content: space-between;
    padding: 0 40px;
    height: 58px;
  }
  .logo {
    font-size: 19px; font-weight: 700; letter-spacing: 1px;
    display: flex; align-items: center; gap: 10px;
  }
  .logo .dot { width: 7px; height: 7px; background: #fff; display: inline-block; }
  .nav-links { display: flex; gap: 28px; }
  .nav-links a {
    color: var(--dim); font-size: 12px; letter-spacing: 2px;
    text-transform: uppercase; text-decoration: none;
    transition: color 0.2s;
  }
  .nav-links a:hover { color: var(--text); }
  .status-pill {
    display: flex; align-items: center; gap: 7px;
    font-family: var(--mono); font-size: 11px; color: var(--dim);
  }
  .status-pill .blink {
    width: 6px; height: 6px; background: #fff; border-radius: 50%;
    animation: blink 1.6s infinite;
  }
  @keyframes blink { 0%,100%{opacity:1} 50%{opacity:0.2} }

  /* ── HERO ── */
  .hero {
    border-bottom: 1px solid var(--border);
    padding: 64px 40px 56px;
    display: grid; grid-template-columns: 1fr 1fr; gap: 60px; align-items: center;
  }
  .hero-left h1 {
    font-size: clamp(36px, 5vw, 58px);
    line-height: 1.05; letter-spacing: -1.5px; font-weight: 700;
    margin-bottom: 18px;
  }
  .hero-left h1 em { font-style: italic; font-weight: 400; }
  .hero-left p { color: var(--dim); font-size: 15px; line-height: 1.7; max-width: 420px; }
  .hero-right {
    border: 1px solid var(--border);
    padding: 24px;
    font-family: var(--mono);
    font-size: 12px;
    line-height: 1.9;
    color: var(--text2);
    position: relative;
  }
  .hero-right::before {
    content: 'terminal';
    position: absolute; top: -10px; left: 16px;
    background: var(--bg); padding: 0 6px;
    font-size: 10px; letter-spacing: 2px; text-transform: uppercase; color: var(--dim);
  }
  .cmd { color: var(--dim); }
  .cmd span { color: #fff; }
  .blinking-cursor { animation: blink 0.9s infinite; }

  /* ── STATS BAR ── */
  .stats-bar {
    display: grid; grid-template-columns: repeat(4,1fr);
    border-bottom: 1px solid var(--border);
  }
  .stat-item {
    padding: 24px 40px;
    border-right: 1px solid var(--border);
  }
  .stat-item:last-child { border-right: none; }
  .stat-val {
    font-size: 32px; font-weight: 700; letter-spacing: -1px;
    font-family: var(--mono);
  }
  .stat-label {
    font-size: 10px; letter-spacing: 2.5px; text-transform: uppercase;
    color: var(--dim); margin-top: 4px;
  }

  /* ── MAIN LAYOUT ── */
  .main { display: grid; grid-template-columns: 340px 1fr; min-height: calc(100vh - 58px); }
  .sidebar {
    border-right: 1px solid var(--border);
    padding: 32px 28px;
    position: sticky; top: 58px; height: calc(100vh - 58px);
    overflow-y: auto;
    display: flex; flex-direction: column; gap: 24px;
  }
  .content { padding: 32px 40px; }

  /* ── FORM ── */
  .form-title {
    font-size: 10px; letter-spacing: 3px; text-transform: uppercase;
    color: var(--dim); margin-bottom: 16px;
    padding-bottom: 10px; border-bottom: 1px solid var(--border);
  }
  .field { margin-bottom: 14px; }
  .field label {
    display: block; font-size: 10px; letter-spacing: 2px;
    text-transform: uppercase; color: var(--dim); margin-bottom: 6px;
  }
  .field input, .field textarea, .field select {
    width: 100%; padding: 10px 12px;
    background: var(--bg3); border: 1px solid var(--border);
    color: var(--text); font-family: var(--font); font-size: 14px;
    outline: none; resize: vertical; transition: border-color 0.2s;
  }
  .field input:focus, .field textarea:focus, .field select:focus {
    border-color: var(--border2);
    background: #0f0f0f;
  }
  .field textarea { min-height: 80px; }
  .field select option { background: var(--bg3); }

  .btn {
    width: 100%; padding: 11px;
    font-family: var(--font); font-size: 13px; letter-spacing: 1.5px;
    text-transform: uppercase; cursor: pointer;
    border: 1px solid var(--text); background: var(--text);
    color: var(--bg); transition: all 0.2s;
  }
  .btn:hover { background: var(--bg3); color: var(--text); }
  .btn:active { transform: scale(0.98); }

  /* ── FILTER BAR ── */
  .filter-bar {
    display: flex; align-items: center; gap: 12px;
    margin-bottom: 28px; flex-wrap: wrap;
  }
  .filter-bar label {
    font-size: 10px; letter-spacing: 2px; text-transform: uppercase; color: var(--dim);
  }
  .filter-btn {
    padding: 5px 14px; border: 1px solid var(--border);
    background: transparent; color: var(--dim);
    font-family: var(--font); font-size: 11px; letter-spacing: 1px;
    cursor: pointer; text-transform: uppercase; transition: all 0.15s;
  }
  .filter-btn.active, .filter-btn:hover {
    border-color: var(--text2); color: var(--text);
  }
  .search-input {
    margin-left: auto; padding: 6px 12px;
    background: var(--bg3); border: 1px solid var(--border);
    color: var(--text); font-family: var(--mono); font-size: 12px; outline: none;
    width: 180px; transition: border-color 0.2s;
  }
  .search-input:focus { border-color: var(--border2); }

  /* ── NOTES GRID ── */
  .section-header {
    display: flex; align-items: baseline; justify-content: space-between;
    margin-bottom: 22px;
    padding-bottom: 12px; border-bottom: 1px solid var(--border);
  }
  .section-header h2 { font-size: 15px; letter-spacing: 1px; font-weight: 700; }
  .section-header .count { font-family: var(--mono); font-size: 12px; color: var(--dim); }

  .notes-grid {
    display: grid; grid-template-columns: repeat(auto-fill, minmax(260px,1fr));
    gap: 18px;
  }
  .note-card {
    border: 1px solid var(--border);
    padding: 22px 20px; cursor: default;
    transition: border-color 0.2s, transform 0.15s;
    position: relative; display: flex; flex-direction: column; gap: 10px;
    animation: fadeUp 0.35s ease both;
  }
  .note-card:hover { border-color: var(--border2); transform: translateY(-2px); }
  @keyframes fadeUp { from { opacity:0; transform: translateY(12px); } to { opacity:1; transform: none; } }

  .note-card .tag {
    font-family: var(--mono); font-size: 10px; letter-spacing: 2px;
    text-transform: uppercase; color: var(--dim);
    display: flex; align-items: center; justify-content: space-between;
  }
  .note-card h3 { font-size: 16px; font-weight: 700; line-height: 1.3; }
  .note-card p  { font-size: 13px; color: var(--text2); line-height: 1.6; flex: 1; }
  .note-card .meta {
    font-family: var(--mono); font-size: 10px; color: var(--dim);
    border-top: 1px solid var(--border); padding-top: 10px;
    display: flex; align-items: center; justify-content: space-between;
  }
  .delete-btn {
    background: none; border: none; cursor: pointer;
    color: var(--dim); font-size: 16px; padding: 0 2px;
    transition: color 0.15s; line-height: 1;
  }
  .delete-btn:hover { color: #fff; }

  .empty-state {
    grid-column: 1/-1;
    border: 1px dashed var(--border);
    padding: 60px; text-align: center;
    color: var(--dim); font-style: italic; font-size: 15px;
  }

  /* ── API EXPLORER ── */
  .api-section { margin-top: 48px; }
  .api-section h2 { font-size: 15px; letter-spacing: 1px; margin-bottom: 20px; padding-bottom: 12px; border-bottom: 1px solid var(--border); }
  .endpoint-list { display: flex; flex-direction: column; gap: 10px; }
  .endpoint {
    border: 1px solid var(--border); padding: 14px 18px;
    display: flex; align-items: center; gap: 16px; font-family: var(--mono);
    font-size: 12px; transition: border-color 0.2s;
  }
  .endpoint:hover { border-color: var(--border2); }
  .method {
    padding: 3px 10px; font-size: 10px; letter-spacing: 1px;
    font-weight: bold; border: 1px solid var(--border2);
  }
  .method.GET  { color: #ccc; }
  .method.POST { color: #fff; background: #1a1a1a; }
  .method.DEL  { color: #888; border-style: dashed; }
  .endpoint-path { color: var(--text2); flex: 1; }
  .endpoint-desc { color: var(--dim); font-size: 11px; }

  /* ── TOAST ── */
  #toast {
    position: fixed; bottom: 28px; right: 28px; z-index: 999;
    background: #fff; color: #000;
    padding: 12px 22px; font-size: 13px; letter-spacing: 0.5px;
    border: 1px solid #fff;
    transform: translateY(80px); opacity: 0;
    transition: all 0.3s cubic-bezier(.25,.8,.25,1);
    pointer-events: none; font-family: 'Times New Roman', serif;
  }
  #toast.show { transform: translateY(0); opacity: 1; }

  /* ── FOOTER ── */
  footer {
    border-top: 1px solid var(--border);
    padding: 28px 40px;
    display: flex; align-items: center; justify-content: space-between;
    font-family: var(--mono); font-size: 11px; color: var(--dim);
  }

  @media (max-width: 900px) {
    .main { grid-template-columns: 1fr; }
    .sidebar { position: static; height: auto; }
    .hero { grid-template-columns: 1fr; }
    .stats-bar { grid-template-columns: repeat(2,1fr); }
    header { padding: 0 20px; }
    .hero, .content { padding: 32px 20px; }
  }
</style>
</head>
<body>

<!-- ── HEADER ── -->
<header>
  <div class="logo">
    <div class="dot"></div>
    NoteOS
  </div>
  <nav class="nav-links">
    <a href="#notes">Notes</a>
    <a href="#api">API</a>
    <a href="#about">About</a>
  </nav>
  <div class="status-pill">
    <div class="blink"></div>
    <span id="serverStatus">C++ Server · Port 8080</span>
  </div>
</header>

<!-- ── HERO ── -->
<section class="hero">
  <div class="hero-left">
    <h1>A <em>full-stack</em><br>app built in<br>pure C++.</h1>
    <p>No Node. No Python. No frameworks. Just raw POSIX sockets, in-memory storage, and a hand-rolled HTTP/1.1 server — compiled and served entirely in C++17.</p>
  </div>
  <div class="hero-right">
    <div class="cmd">$ <span>./server 8080</span></div>
    <div style="color:#555">──────────────────────────────</div>
    <div class="cmd">  <span style="color:#888">▸</span> HTTP server initializing...</div>
    <div class="cmd">  <span style="color:#888">▸</span> Socket bound to 0.0.0.0:8080</div>
    <div class="cmd">  <span style="color:#888">▸</span> Thread pool ready</div>
    <div class="cmd">  <span style="color:#ccc">▸</span> Listening for connections</div>
    <div style="color:#555">──────────────────────────────</div>
    <div class="cmd">  Built with: <span>g++ -std=c++17</span></div>
    <div class="cmd">  Stack: <span>C++ · POSIX · JSON</span></div>
    <div id="uptimeLine" class="cmd">  Uptime: <span id="uptimeVal">00h 00m 00s</span><span class="blinking-cursor">_</span></div>
  </div>
</section>

<!-- ── STATS BAR ── -->
<section class="stats-bar">
  <div class="stat-item">
    <div class="stat-val" id="statNotes">0</div>
    <div class="stat-label">Total Notes</div>
  </div>
  <div class="stat-item">
    <div class="stat-val" id="statRequests">0</div>
    <div class="stat-label">HTTP Requests</div>
  </div>
  <div class="stat-item">
    <div class="stat-val" id="statUptime">0s</div>
    <div class="stat-label">Server Uptime</div>
  </div>
  <div class="stat-item">
    <div class="stat-val" id="statBytes">0B</div>
    <div class="stat-label">Bytes Served</div>
  </div>
</section>

<!-- ── MAIN ── -->
<main class="main" id="notes">

  <!-- SIDEBAR: CREATE NOTE -->
  <aside class="sidebar">
    <div>
      <div class="form-title">§ Create New Note</div>
      <div class="field">
        <label>Title</label>
        <input type="text" id="noteTitle" placeholder="Enter note title...">
      </div>
      <div class="field">
        <label>Body</label>
        <textarea id="noteBody" placeholder="Write your note here..."></textarea>
      </div>
      <div class="field">
        <label>Tag</label>
        <input type="text" id="noteTag" placeholder="e.g. work, personal, idea">
      </div>
      <div class="field">
        <label>Style</label>
        <select id="noteColor">
          <option value="default">Default — Border only</option>
          <option value="filled">Filled — White card</option>
        </select>
      </div>
      <button class="btn" onclick="createNote()">+ Post Note</button>
    </div>

    <div>
      <div class="form-title" style="margin-top:8px;">§ Quick Commands</div>
      <div style="font-family:var(--mono);font-size:11px;color:var(--dim);line-height:2;">
        <div>GET  /api/notes</div>
        <div>POST /api/notes</div>
        <div>DEL  /api/notes/:id</div>
        <div>GET  /api/stats</div>
        <div>GET  /api/health</div>
      </div>
    </div>
  </aside>

  <!-- CONTENT -->
  <section class="content">

    <!-- FILTER BAR -->
    <div class="filter-bar">
      <label>Filter:</label>
      <button class="filter-btn active" onclick="setFilter('all',this)">All</button>
      <button class="filter-btn" onclick="setFilter('work',this)">Work</button>
      <button class="filter-btn" onclick="setFilter('personal',this)">Personal</button>
      <button class="filter-btn" onclick="setFilter('idea',this)">Idea</button>
      <input class="search-input" id="searchInput" placeholder="search notes..." oninput="renderNotes()">
    </div>

    <!-- NOTES -->
    <div class="section-header">
      <h2>Notes Board</h2>
      <span class="count" id="noteCount">0 notes</span>
    </div>
    <div class="notes-grid" id="notesGrid">
      <div class="empty-state">Loading notes from C++ server...</div>
    </div>

    <!-- API EXPLORER -->
    <div class="api-section" id="api">
      <h2>§ REST API Reference</h2>
      <div class="endpoint-list">
        <div class="endpoint">
          <span class="method GET">GET</span>
          <span class="endpoint-path">/api/notes</span>
          <span class="endpoint-desc">Returns all notes as JSON array</span>
        </div>
        <div class="endpoint">
          <span class="method POST">POST</span>
          <span class="endpoint-path">/api/notes</span>
          <span class="endpoint-desc">Create a new note · Body: {title, body, tag, color}</span>
        </div>
        <div class="endpoint">
          <span class="method DEL">DEL</span>
          <span class="endpoint-path">/api/notes/:id</span>
          <span class="endpoint-desc">Delete note by ID</span>
        </div>
        <div class="endpoint">
          <span class="method GET">GET</span>
          <span class="endpoint-path">/api/stats</span>
          <span class="endpoint-desc">Server stats · uptime, requests, bytes</span>
        </div>
        <div class="endpoint">
          <span class="method GET">GET</span>
          <span class="endpoint-path">/api/health</span>
          <span class="endpoint-desc">Health check · returns {status: "ok"}</span>
        </div>
      </div>
    </div>

  </section>
</main>

<!-- ── FOOTER ── -->
<footer id="about">
  <div>© NoteOS — Full-Stack C++17 · POSIX Sockets · No external dependencies</div>
  <div id="footerStats">Server initializing...</div>
</footer>

<!-- ── TOAST ── -->
<div id="toast"></div>

<script>
  let allNotes = [];
  let activeFilter = 'all';

  function toast(msg) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.classList.add('show');
    setTimeout(() => t.classList.remove('show'), 2800);
  }

  async function fetchNotes() {
    try {
      const r = await fetch('/api/notes');
      allNotes = await r.json();
      renderNotes();
    } catch(e) { console.error(e); }
  }

  async function fetchStats() {
    try {
      const r = await fetch('/api/stats');
      const s = await r.json();
      document.getElementById('statNotes').textContent    = s.total_notes;
      document.getElementById('statRequests').textContent = s.requests;
      document.getElementById('statBytes').textContent    = formatBytes(s.bytes_served);
      document.getElementById('statUptime').textContent   = s.uptime;
      document.getElementById('uptimeVal').textContent    = s.uptime;
      document.getElementById('footerStats').textContent  = `Requests: ${s.requests} · Uptime: ${s.uptime}`;
    } catch(e) {}
  }

  function formatBytes(b) {
    if (b < 1024) return b+'B';
    if (b < 1048576) return (b/1024).toFixed(1)+'KB';
    return (b/1048576).toFixed(2)+'MB';
  }

  function setFilter(f, btn) {
    activeFilter = f;
    document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    renderNotes();
  }

  function renderNotes() {
    const query = document.getElementById('searchInput').value.toLowerCase();
    let notes = allNotes;
    if (activeFilter !== 'all') {
      notes = notes.filter(n => (n.tag||'').toLowerCase().includes(activeFilter));
    }
    if (query) {
      notes = notes.filter(n =>
        n.title.toLowerCase().includes(query) ||
        n.body.toLowerCase().includes(query) ||
        (n.tag||'').toLowerCase().includes(query)
      );
    }
    document.getElementById('noteCount').textContent = notes.length + ' note' + (notes.length!==1?'s':'');
    const grid = document.getElementById('notesGrid');
    if (!notes.length) {
      grid.innerHTML = '<div class="empty-state">No notes found. Create one to get started.</div>';
      return;
    }
    grid.innerHTML = notes.map((n,i) => {
      const filled = n.color === 'filled';
      const cardStyle = filled
        ? 'background:#fff;color:#000;border-color:#fff;'
        : '';
      const dimColor = filled ? '#444' : 'var(--dim)';
      const borderColor = filled ? '#ddd' : 'var(--border)';
      return `<div class="note-card" style="${cardStyle}animation-delay:${i*0.05}s">
        <div class="tag" style="color:${dimColor}">
          <span>${n.tag ? '#'+n.tag : '#untagged'}</span>
          <button class="delete-btn" onclick="deleteNote(${n.id})" style="color:${dimColor}" title="Delete">×</button>
        </div>
        <h3>${escHtml(n.title)}</h3>
        <p>${escHtml(n.body)}</p>
        <div class="meta" style="color:${dimColor};border-top-color:${borderColor}">
          <span>id:${n.id}</span>
          <span>${n.created_at.split(' ').slice(1,4).join(' ')}</span>
        </div>
      </div>`;
    }).join('');
  }

  function escHtml(s) {
    return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
  }

  async function createNote() {
    const title = document.getElementById('noteTitle').value.trim();
    const body  = document.getElementById('noteBody').value.trim();
    const tag   = document.getElementById('noteTag').value.trim();
    const color = document.getElementById('noteColor').value;
    if (!title) { toast('Title cannot be empty.'); return; }
    if (!body)  { toast('Body cannot be empty.');  return; }
    try {
      const r = await fetch('/api/notes', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ title, body, tag, color })
      });
      if (r.ok) {
        document.getElementById('noteTitle').value = '';
        document.getElementById('noteBody').value  = '';
        document.getElementById('noteTag').value   = '';
        await fetchNotes();
        toast('Note created successfully.');
      }
    } catch(e) { toast('Error: Could not reach server.'); }
  }

  async function deleteNote(id) {
    try {
      await fetch('/api/notes/' + id, { method: 'DELETE' });
      await fetchNotes();
      toast('Note deleted.');
    } catch(e) {}
  }

  // Init
  fetchNotes();
  fetchStats();
  setInterval(fetchStats, 3000);
</script>
</body>
</html>
)HTMLEOF";

// ─── ROUTE HANDLERS ───────────────────────────────────────────────────────────
HttpResponse handle_get_notes() {
    std::lock_guard<std::mutex> lock(db_mutex);
    HttpResponse res;
    res.body = notes_array_json(notes_db);
    return res;
}

HttpResponse handle_post_note(const HttpRequest& req) {
    HttpResponse res;
    std::string title = json_get(req.body, "title");
    std::string body  = json_get(req.body, "body");
    std::string tag   = json_get(req.body, "tag");
    std::string color = json_get(req.body, "color");
    if (title.empty() || body.empty()) {
        res.status = 400;
        res.body   = "{\"error\":\"title and body are required\"}";
        return res;
    }
    Note n;
    n.id         = id_counter++;
    n.title      = title;
    n.body       = body;
    n.tag        = tag.empty() ? "general" : tag;
    n.color      = color.empty() ? "default" : color;
    n.created_at = current_time_str();
    {
        std::lock_guard<std::mutex> lock(db_mutex);
        notes_db.push_back(n);
    }
    res.status = 201;
    res.body   = note_to_json(n);
    return res;
}

HttpResponse handle_delete_note(const std::string& path) {
    HttpResponse res;
    // Extract ID from /api/notes/:id
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        res.status = 400;
        res.body   = "{\"error\":\"invalid path\"}";
        return res;
    }
    int id = 0;
    try { id = std::stoi(path.substr(slash+1)); }
    catch (...) {
        res.status = 400;
        res.body   = "{\"error\":\"invalid id\"}";
        return res;
    }
    std::lock_guard<std::mutex> lock(db_mutex);
    auto it = std::find_if(notes_db.begin(), notes_db.end(),
                           [id](const Note& n){ return n.id == id; });
    if (it == notes_db.end()) {
        res.status = 404;
        res.body   = "{\"error\":\"note not found\"}";
        return res;
    }
    notes_db.erase(it);
    res.status = 200;
    res.body   = "{\"deleted\":true}";
    return res;
}

HttpResponse handle_stats() {
    HttpResponse res;
    std::ostringstream o;
    o << "{"
      << "\"total_notes\":"  << notes_db.size()       << ","
      << "\"requests\":"     << request_count.load()  << ","
      << "\"bytes_served\":" << bytes_served.load()   << ","
      << "\"uptime\":\""     << uptime_str()          << "\""
      << "}";
    res.body = o.str();
    return res;
}

HttpResponse handle_health() {
    HttpResponse res;
    res.body = "{\"status\":\"ok\",\"server\":\"C++ NoteOS\",\"version\":\"1.0.0\"}";
    return res;
}

HttpResponse route(const HttpRequest& req) {
    HttpResponse res;

    // CORS preflight
    if (req.method == "OPTIONS") {
        res.status = 204;
        res.body   = "";
        return res;
    }

    // Serve frontend
    if (req.path == "/" || req.path == "/index.html") {
        res.content_type = "text/html; charset=utf-8";
        res.body = FRONTEND_HTML;
        return res;
    }

    // API routes
    if (req.path == "/api/notes") {
        if (req.method == "GET")  return handle_get_notes();
        if (req.method == "POST") return handle_post_note(req);
        res.status = 405;
        res.body   = "{\"error\":\"method not allowed\"}";
        return res;
    }
    if (req.path.substr(0, 11) == "/api/notes/") {
        if (req.method == "DELETE") return handle_delete_note(req.path);
        res.status = 405;
        res.body   = "{\"error\":\"method not allowed\"}";
        return res;
    }
    if (req.path == "/api/stats")  return handle_stats();
    if (req.path == "/api/health") return handle_health();

    res.status = 404;
    res.body   = "{\"error\":\"not found\",\"path\":\"" + json_escape(req.path) + "\"}";
    return res;
}

// ─── CLIENT HANDLER (each connection in its own thread) ───────────────────────
void handle_client(int client_fd, const std::string& client_ip) {
    // Read request
    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    ssize_t n;
    while ((n = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
        raw.append(buf, n);
        // Stop reading once we have headers + body
        size_t hdr_end = raw.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            // Check Content-Length to get full body
            size_t cl_pos = raw.find("Content-Length:");
            if (cl_pos == std::string::npos) cl_pos = raw.find("content-length:");
            if (cl_pos != std::string::npos) {
                size_t val_start = cl_pos + 15;
                while (val_start < raw.size() && raw[val_start]==' ') ++val_start;
                size_t val_end = raw.find("\r\n", val_start);
                int cl = std::stoi(raw.substr(val_start, val_end-val_start));
                size_t body_start = hdr_end + 4;
                if ((int)(raw.size() - body_start) >= cl) break;
            } else {
                break;
            }
        }
        if (n < (ssize_t)sizeof(buf)) break;
    }

    if (raw.empty()) { close(client_fd); return; }

    HttpRequest  req = parse_request(raw);
    HttpResponse res = route(req);

    std::string response_str = build_response(res);
    send(client_fd, response_str.c_str(), response_str.size(), 0);

    request_count++;
    bytes_served += (long long)response_str.size();

    // Terminal log
    const char* method_color = BLU;
    if (req.method == "POST")   method_color = GRN;
    if (req.method == "DELETE") method_color = RED;

    std::cout << DIM << "[" << current_time_str().substr(11,8) << "] " << RST
              << method_color << BLD << req.method << RST
              << std::string(8 - req.method.size(), ' ')
              << CYN << req.path << RST
              << std::string(std::max(1,(int)(36-req.path.size())), ' ')
              << (res.status < 300 ? GRN : (res.status < 400 ? YLW : RED))
              << res.status << RST
              << DIM << "  " << client_ip << RST << "\n";

    close(client_fd);
}

// ─── SEED DATA ────────────────────────────────────────────────────────────────
void seed_notes() {
    auto add = [](const std::string& t, const std::string& b,
                  const std::string& tag, const std::string& c) {
        Note n;
        n.id         = id_counter++;
        n.title      = t;  n.body = b;
        n.tag        = tag; n.color = c;
        n.created_at = current_time_str();
        notes_db.push_back(n);
    };
    add("Welcome to NoteOS",
        "This full-stack app is written entirely in C++17. The backend uses raw POSIX sockets — no frameworks.",
        "meta", "filled");
    add("The HTTP Server",
        "Each client connection is handled in a dedicated thread. The request parser is hand-rolled with zero external deps.",
        "idea", "default");
    add("REST API Design",
        "POST /api/notes to create · GET to list all · DELETE /api/notes/:id to remove. Stats at /api/stats.",
        "work", "default");
    add("In-Memory Storage",
        "Notes are stored in a std::vector protected by std::mutex. Fast, simple, thread-safe.",
        "work", "default");
}

// ─── SIGNAL HANDLER ──────────────────────────────────────────────────────────
volatile sig_atomic_t running = 1;
void sig_handler(int) {
    running = 0;
    std::cout << "\n" << YLW << "  ▸ Shutting down gracefully..." << RST << "\n";
}

// ─── MAIN ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 8080;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    server_start = std::chrono::steady_clock::now();
    seed_notes();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << RED << "  ✗ Failed to create socket\n" << RST;
        return 1;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << RED << "  ✗ Bind failed on port " << port << RST << "\n";
        return 1;
    }
    listen(server_fd, 64);

    // Banner
    std::cout << "\n";
    std::cout << BLD << "  ╔══════════════════════════════════════╗\n";
    std::cout << "  ║         NoteOS — C++ Full-Stack      ║\n";
    std::cout << "  ╚══════════════════════════════════════╝\n" << RST;
    std::cout << "\n";
    std::cout << GRN << "  ▸ " << RST << "HTTP server listening on "
              << BLD << "http://localhost:" << port << RST << "\n";
    std::cout << GRN << "  ▸ " << RST << "Pre-loaded " << notes_db.size() << " seed notes\n";
    std::cout << GRN << "  ▸ " << RST << "REST API at " << CYN << "/api/notes" << RST << "\n";
    std::cout << GRN << "  ▸ " << RST << "Stats at    " << CYN << "/api/stats" << RST << "\n";
    std::cout << DIM << "  ─────────────────────────────────────\n" << RST;
    std::cout << DIM << "  Press Ctrl+C to stop\n\n" << RST;

    while (running) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        // Spawn thread per connection (thread-per-request model)
        std::thread([client_fd, client_ip]() {
            handle_client(client_fd, client_ip);
        }).detach();
    }

    close(server_fd);
    std::cout << GRN << "  ▸ Server stopped. Goodbye.\n\n" << RST;
    return 0;
}
