#include "port_update_check.h"

#include "port_json.h"
#include "port_log.h"
#include "port_ws.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

namespace PortUpdateCheck {
namespace {

// The state file's name is UTF-8; a narrow path is the ANSI code page on Windows.
std::filesystem::path Utf8Path(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool SplitVersion(const std::string& text, std::vector<uint32_t>& parts) {
  parts.clear();
  size_t at = !text.empty() && (text[0] == 'v' || text[0] == 'V') ? 1 : 0;
  if (at >= text.size()) {
    return false;
  }
  uint64_t value = 0;
  bool digits = false;
  for (; at <= text.size(); ++at) {
    const char c = at < text.size() ? text[at] : '.';
    if (c >= '0' && c <= '9') {
      value = value * 10 + static_cast<uint32_t>(c - '0');
      if (value > 0xffffffffu) {
        return false;
      }
      digits = true;
    } else if (c == '.' && digits) {
      parts.push_back(static_cast<uint32_t>(value));
      value = 0;
      digits = false;
    } else {
      return false;
    }
  }
  return true;
}

// Only ever opened in a browser: keep it to this repository's pages.
std::string SafeReleaseUrl(const std::string& url) {
  const std::string prefix = std::string("https://github.com/") + kRepo + "/";
  bool ok = url.compare(0, prefix.size(), prefix) == 0;
  for (const char c : url) {
    ok = ok && c > ' ' && c < 0x7f && c != '"';
  }
  return ok ? url : std::string("https://github.com/") + kRepo + "/releases/latest";
}

int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

struct Runtime {
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  std::atomic<bool> cancel{false};
  bool enabled = false;
  bool stop = false;
  bool workerDone = false;
  bool forceCheck = false;
  std::string current;
  std::string stateFile;
  EStatus status = kStatus_Off;
  Release latest;
  std::string error;
  int64_t checked = 0;

  // checked=<unix time>, latest=<version>, url=<release page>.
  void LoadState() {
    std::ifstream in(Utf8Path(stateFile));
    std::string line;
    while (std::getline(in, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const std::string key = line.substr(0, eq);
      const std::string value = line.substr(eq + 1);
      if (key == "checked") {
        checked = std::strtoll(value.c_str(), nullptr, 10);
      } else if (key == "latest") {
        latest.version = value;
      } else if (key == "url") {
        latest.url = SafeReleaseUrl(value);
      }
    }
  }

  void SaveState() const {
    const std::filesystem::path file = Utf8Path(stateFile);
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
      std::ofstream out(temp, std::ios::trunc);
      out << "checked=" << checked << "\nlatest=" << latest.version << "\nurl=" << latest.url << '\n';
      if (!out) {
        return;
      }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec); // replaces the old file, on Windows too
  }

  // With the lock held.
  void UpdateStatusFromLatest() {
    status = !latest.version.empty() && CompareVersions(latest.version, current) > 0 ? kStatus_Available
                                                                                     : kStatus_UpToDate;
  }

  static bool Fetch(const std::string& version, std::atomic<bool>& cancel, Release& out, std::string& why) {
    PortWs::Client client;
    client.SetCancelFlag(&cancel);
    const std::string headers = "User-Agent: metroid-prime-port/" + version +
                                "\r\nAccept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    int status = 0;
    std::string body;
    if (!client.HttpGet("api.github.com", 443, std::string("/repos/") + kRepo + "/releases/latest", 15000, true,
                        headers, status, body, 1u << 20)) {
      why = client.Error();
      return false;
    }
    if (status != 200) {
      why = "GitHub answered HTTP " + std::to_string(status);
      return false;
    }
    if (!ParseLatestRelease(body, out)) {
      why = "unexpected reply from GitHub";
      return false;
    }
    return true;
  }

  // Checks only when asked: once at launch (debug_ui calls CheckNow) and on "Check now".
  void Run() {
    std::unique_lock<std::mutex> lock(mutex);
    while (!stop) {
      if (!enabled || !forceCheck) {
        wake.wait(lock);
        continue;
      }
      forceCheck = false;
      status = kStatus_Checking;
      const std::string version = current;
      lock.unlock();
      Release release;
      std::string why;
      const bool ok = Fetch(version, cancel, release, why);
      lock.lock();
      if (stop) {
        break;
      }
      if (!enabled) {
        continue; // switched off mid-check: keep it off
      }
      if (ok) {
        PortLog::Write("port: update check: latest release %s, this is %s\n", release.version.c_str(),
                       current.c_str());
        latest = release;
        checked = Now();
        error.clear();
        SaveState();
        UpdateStatusFromLatest();
      } else {
        PortLog::Write("port: update check failed: %s\n", why.c_str());
        error = why;
        status = kStatus_Failed;
      }
    }
    workerDone = true;
    wake.notify_all();
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cancel = true;
    wake.notify_all();
    if (!worker.joinable()) {
      return;
    }
    bool done;
    {
      std::unique_lock<std::mutex> lock(mutex);
      done = wake.wait_for(lock, std::chrono::seconds(2), [this] { return workerDone; });
    }
    if (done) {
      worker.join();
    } else {
      worker.detach();
    }
  }
};

Runtime& GetRuntime() {
  // Leaked, so a worker still in a request at exit never sees it destroyed.
  static Runtime* runtime = new Runtime;
  static struct Stopper {
    Runtime* runtime;
    ~Stopper() { runtime->Shutdown(); }
  } stopper{runtime};
  return *runtime;
}

} // namespace

int CompareVersions(const std::string& a, const std::string& b) {
  std::vector<uint32_t> left;
  std::vector<uint32_t> right;
  const bool leftOk = SplitVersion(a, left);
  const bool rightOk = SplitVersion(b, right);
  if (!leftOk || !rightOk) {
    return static_cast<int>(leftOk) - static_cast<int>(rightOk);
  }
  const size_t count = std::max(left.size(), right.size());
  for (size_t i = 0; i < count; ++i) {
    const uint32_t l = i < left.size() ? left[i] : 0;
    const uint32_t r = i < right.size() ? right[i] : 0;
    if (l != r) {
      return l < r ? -1 : 1;
    }
  }
  return 0;
}

bool ParseLatestRelease(const std::string& json, Release& out) {
  PortJson::Value root;
  size_t errorOffset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(json, root, errorOffset, &reason) || !root.IsObject()) {
    return false;
  }
  if (const PortJson::Value* draft = root.Find("draft"); draft != nullptr && draft->AsBool()) {
    return false;
  }
  std::string tag = root.StringOr("tag_name");
  std::vector<uint32_t> parts;
  if (!SplitVersion(tag, parts)) {
    return false;
  }
  if (tag[0] == 'v' || tag[0] == 'V') {
    tag.erase(0, 1);
  }
  out.version = tag;
  out.url = SafeReleaseUrl(root.StringOr("html_url"));
  return true;
}

void Configure(bool enabled, const std::string& current, const std::string& stateFile) {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  enabled = enabled && !current.empty() && !stateFile.empty();
  if (rt.stateFile != stateFile) {
    rt.stateFile = stateFile;
    rt.latest = Release{};
    rt.checked = 0;
    if (!stateFile.empty()) {
      rt.LoadState();
    }
  }
  rt.current = current;
  rt.enabled = enabled;
  if (!enabled) {
    rt.status = kStatus_Off;
  } else if (rt.status != kStatus_Checking && rt.status != kStatus_Failed) {
    rt.UpdateStatusFromLatest();
  }
  if (enabled && !rt.worker.joinable() && !rt.stop) {
    rt.worker = std::thread([&rt] { rt.Run(); });
  }
  rt.wake.notify_all();
}

void CheckNow() {
  Runtime& rt = GetRuntime();
  {
    std::lock_guard<std::mutex> lock(rt.mutex);
    if (!rt.enabled || rt.status == kStatus_Checking) {
      return;
    }
    rt.forceCheck = true;
  }
  rt.wake.notify_all();
}

EStatus Status() {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  return rt.status;
}

Release Latest() {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  return rt.latest;
}

std::string LastError() {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  return rt.error;
}

int64_t LastChecked() {
  Runtime& rt = GetRuntime();
  std::lock_guard<std::mutex> lock(rt.mutex);
  return rt.checked;
}

} // namespace PortUpdateCheck
