#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <base64.h>

#include <memory>
#include <string>
#include <utility>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

// KOSync's TLS-1.3 servers can't be reached through the precompiled system
// mbedTLS (TLS 1.3 is stubbed out), so requests run over wolfSSL via
// SecureHttpClient. The handshake still needs working heap; gate on it. wolfSSL's
// footprint is smaller than mbedTLS's old ~48KB peak, but keep a conservative
// floor. Check both total free heap and largest contiguous block so fragmented
// heap does not fall through into a failed TLS allocation path.
// MEMFIX-PORT: TLS heap gate; portable
// Field data (July 2026): launching sync from a reader session lands at
// 51.9-58.2 KB free / 42-53 KB maxAlloc after WiFi comes up. wolfSSL handles
// allocation failure by returning MEMORY_E (no abort under -fno-exceptions),
// so an optimistic attempt degrades to the same clean "sync failed" as the
// gate — the gate only needs to keep out states where a doomed handshake
// would waste tens of seconds, not guarantee success.
//
// Free and largest-block have separate requirements: with SP ECC
// (WOLFSSL_HAVE_SP_ECC) the handshake's crypto uses fixed 256-bit arrays, so
// the largest single TLS allocation is the ~17 KB wolfSSL record buffer, not
// a run of fast-math bignums. A handshake was measured succeeding inside a
// 43 KB largest block; requiring 50 KB contiguous refused syncs that fit.
//
// The 35 KB free floor covers the measured peak of what remains after the SP
// ECC + X25519 work: session object plus record buffer plus RSA cert-verify
// temps (2 KB apiece at FP_MAX_BITS 8192) totals ~30-40 KB transient. The old
// 50 KB floor was calibrated against the fast-math bignum failure mode that
// SP ECC removed, and sat inside the 51.9-58.2 KB band a reading session
// normally leaves, refusing syncs that would have succeeded. A wrong guess
// here fails soft: MEMORY_E aborts the handshake within its 15 s deadline.
constexpr uint32_t MIN_FREE_FOR_TLS = 35000;
constexpr uint32_t MIN_BLOCK_FOR_TLS = 20000;

// Apply the shared KOSync auth headers after begin(). x-auth-* is the native
// KOSync scheme; Basic auth is added for Calibre-Web-Automated compatibility.
void applyAuthHeaders(freeink::SecureHttpClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  const std::string credentials = KOREADER_STORE.getUsername() + ":" + KOREADER_STORE.getPassword();
  const String encoded = base64::encode(credentials.c_str());
  http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
}

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_FOR_TLS || maxAllocHeap < MIN_BLOCK_FOR_TLS) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_FOR_TLS, maxAllocHeap, MIN_BLOCK_FOR_TLS);
    return true;
  }
  return false;
}

bool deadlineReached(const uint32_t deadlineAt) { return static_cast<int32_t>(millis() - deadlineAt) >= 0; }
}  // namespace

struct KOReaderSyncHttpSession::Impl {
  freeink::SecureHttpClient http;
};

KOReaderSyncHttpSession::KOReaderSyncHttpSession() : impl(makeUniqueNoThrow<Impl>()) {}

KOReaderSyncHttpSession::~KOReaderSyncHttpSession() = default;

uint32_t KOReaderSyncHttpSession::nowMs() const { return millis(); }

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating (heap: %u)", (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Invalid sync endpoint");
    return NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  const int httpCode = http.GET();
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  // Any 2xx is success. The reference kosync server answers 200, but
  // KOSync-compatible implementations differ (BookLore/grimmory is a Spring
  // service and uses the idiomatic codes) — see issue #2876.
  if (httpCode >= 200 && httpCode < 300) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account (heap: %u)", (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  freeink::SecureHttpClient http;
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Invalid sync endpoint");
    return NETWORK_ERROR;
  }
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.sendRequest("POST", body);
  http.end();
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Create user response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode >= 200 && httpCode < 300) return OK;  // 2xx: created (see #2876)
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress, const uint32_t timeoutMs) {
  KOReaderSyncHttpSession session;
  return session.getProgress(documentHash, outProgress, timeoutMs);
}

KOReaderSyncClient::Error KOReaderSyncHttpSession::getProgress(const std::string& documentHash,
                                                               KOReaderProgress& outProgress,
                                                               const uint32_t timeoutMs) {
  KOReaderSyncClient::lastHttpCode = 0;
  if (!impl) return KOReaderSyncClient::LOW_MEMORY;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return KOReaderSyncClient::NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress (heap: %u)", (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return KOReaderSyncClient::LOW_MEMORY;

  auto& http = impl->http;
  http.setInsecure();
  http.setTimeout(timeoutMs);
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Invalid sync endpoint");
    return KOReaderSyncClient::NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  std::string responseBody;
  const uint32_t deadlineAt = millis() + timeoutMs;
  const int httpCode = http.GET(
      [&responseBody](const uint8_t* data, const size_t length) {
        responseBody.append(reinterpret_cast<const char*>(data), length);
        return true;
      },
      [deadlineAt] { return deadlineReached(deadlineAt); });
  KOReaderSyncClient::lastHttpCode = httpCode;

  if (http.aborted()) {
    http.end();
    LOG_DBG("KOSync", "Get progress deadline expired");
    return KOReaderSyncClient::NETWORK_ERROR;
  }

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode <= 0) {
    return KOReaderSyncClient::NETWORK_ERROR;
  }

  // 204 = success with no stored progress for this document (Spring-style
  // KOSync implementations; the reference server answers 200 with an empty
  // object instead). Map it to the same graceful no-remote-progress path as
  // 404 rather than falling through to SERVER_ERROR — see issue #2876.
  if (httpCode == 204) {
    return KOReaderSyncClient::NOT_FOUND;
  }

  if (httpCode >= 200 && httpCode < 300) {
    JsonDocument doc;
    if (!http.responseComplete()) return KOReaderSyncClient::NETWORK_ERROR;
    const DeserializationError error = deserializeJson(doc, responseBody.c_str());

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return KOReaderSyncClient::JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    outProgress.position.reset();
    if (KOREADER_STORE.usesCrossPointSyncServer()) {
      const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
      if (!pos.isNull()) {
        KOReaderRichPosition rich;
        rich.pctQ = pos["pctQ"].as<uint32_t>();
        rich.spineIndex = pos["spine"].as<uint16_t>();
        rich.pageNumber = pos["page"].as<uint16_t>();
        const uint16_t pages = pos["pages"].as<uint16_t>();
        rich.totalPages = pages > 0 ? pages : 1;
        const uint16_t para = pos["para"].as<uint16_t>();
        if (para > 0) rich.paragraphIndex = para;
        rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
        LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
                rich.totalPages, para);
        outProgress.position = std::move(rich);
      }
    }

    LOG_DBG("KOSync", "Got progress: %.2f%%", outProgress.percentage * 100);
    return KOReaderSyncClient::OK;
  }

  if (httpCode == 401) return KOReaderSyncClient::AUTH_FAILED;
  if (httpCode == 404) return KOReaderSyncClient::NOT_FOUND;
  return KOReaderSyncClient::SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress,
                                                             const uint32_t timeoutMs) {
  KOReaderSyncHttpSession session;
  return session.updateProgress(progress, timeoutMs);
}

KOReaderSyncClient::Error KOReaderSyncHttpSession::updateProgress(const KOReaderProgress& progress,
                                                                  const uint32_t timeoutMs) {
  KOReaderSyncClient::lastHttpCode = 0;
  if (!impl) return KOReaderSyncClient::LOW_MEMORY;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return KOReaderSyncClient::NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress (heap: %u)", (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return KOReaderSyncClient::LOW_MEMORY;

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  if (progress.metadata.has_value()) {
    auto meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;
  if (progress.position.has_value() && KOREADER_STORE.usesCrossPointSyncServer()) {
    // CrossPoint-specific extension: do not send it to third-party KOSync servers.
    const auto& p = *progress.position;
    auto pos = doc["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // Server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }

  std::string body;
  serializeJson(doc, body);

  auto& http = impl->http;
  http.setInsecure();
  http.setTimeout(timeoutMs);
  if (!http.begin(url)) {
    LOG_ERR("KOSync", "Invalid sync endpoint");
    return KOReaderSyncClient::NETWORK_ERROR;
  }
  applyAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");
  const uint32_t deadlineAt = millis() + timeoutMs;
  const int httpCode = http.sendRequest(
      "PUT", reinterpret_cast<const uint8_t*>(body.data()), body.size(), [](const uint8_t*, size_t) { return true; },
      [deadlineAt] { return deadlineReached(deadlineAt); });
  KOReaderSyncClient::lastHttpCode = httpCode;

  if (http.aborted()) {
    http.end();
    LOG_DBG("KOSync", "Update progress deadline expired");
    return KOReaderSyncClient::NETWORK_ERROR;
  }

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode <= 0 || !http.responseComplete()) return KOReaderSyncClient::NETWORK_ERROR;
  // Any 2xx accepts the progress. The reference kosync server answers 200,
  // but Spring-based KOSync implementations (BookLore/grimmory) answer a PUT
  // with the idiomatic 201/204, which used to land in SERVER_ERROR and made
  // every sync against them fail after a successful pull — issue #2876.
  if (httpCode >= 200 && httpCode < 300) return KOReaderSyncClient::OK;
  if (httpCode == 401) return KOReaderSyncClient::AUTH_FAILED;
  return KOReaderSyncClient::SERVER_ERROR;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    default:
      return "Unknown error";
  }
}
