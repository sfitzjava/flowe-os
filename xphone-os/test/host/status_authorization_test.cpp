#include "StatusAuthorization.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
using namespace flowe_status;
Text text(const char* input) { return {input, strlen(input)}; }
constexpr const char* reader = "F85B1BFC2E18";
constexpr const char* other = "F85B1BFC2E19";
void check(const char* sentToken, const char* sentReader, const char* ownerToken,
           bool allowed, bool prepare, bool verified) {
  const auto actual = authorize(text(sentToken), text(sentReader), text(ownerToken), text(reader));
  assert(actual.allowed == allowed && actual.prepareMemory == prepare && actual.sessionVerified == verified);
}
int main() {
  check("owner", reader, "owner", true, true, true);
  check("wrong", reader, "owner", false, false, false);
  check("owner", other, "owner", false, false, false);
  check("owner", "", "owner", false, false, false);
  check("owner", reader, "", false, false, false); // token against guest server
  check("", "", "owner", true, false, false); // owned-server public discovery
  check("", reader, "owner", true, false, false);
  check("", other, "owner", false, false, false);
  check("", other, "", false, false, false);
  check("", "", "", true, true, false); // guest bench remains usable
  check("", reader, "", true, true, false);
  check("Owner", reader, "owner", false, false, false);
  check("owner ", reader, "owner", false, false, false);
  check("owner", "f85b1bfc2e18", "owner", false, false, false);
  const std::string nulToken = std::string("owner") + std::string("\0junk", 5);
  assert(!authorize({nulToken.data(), nulToken.size()}, text(reader), text("owner"), text(reader)).allowed);
  const std::string nulReader = std::string(reader) + std::string("\0junk", 5);
  assert(!authorize(text("owner"), {nulReader.data(), nulReader.size()}, text("owner"), text(reader)).allowed);
  assert(!authorize(text("owner"), text(reader), text("owner"), text("")).allowed); // failed local identity read
  const auto publicNoIdentity = authorize(text(""), text(""), text("owner"), text(""));
  assert(publicNoIdentity.allowed && !publicNoIdentity.prepareMemory && !publicNoIdentity.sessionVerified);
  // Legacy readiness is selected by the encrypted BLE token message only.
  const auto oldAndroid = authorize(text("owner"), text(""), text("owner"), text(reader), false);
  assert(oldAndroid.allowed && oldAndroid.prepareMemory && !oldAndroid.sessionVerified);
  const auto oldIOS = authorize(text(""), text(""), text("owner"), text(reader), false);
  assert(oldIOS.allowed && oldIOS.prepareMemory && !oldIOS.sessionVerified);
  assert(!authorize(text("wrong"), text(""), text("owner"), text(reader), false).allowed);
  assert(!authorize(text("owner"), text(other), text("owner"), text(reader), false).allowed);
  assert(!authorize(text("owner"), text(""), text(""), text(reader), false).allowed);
  assert(!authorize({nulToken.data(), nulToken.size()}, text(""), text("owner"), text(reader), false).allowed);
  assert(!authorize(text("owner"), {nulReader.data(), nulReader.size()}, text("owner"), text(reader), false).allowed);
  puts("status authorization: wrong token, guest token, wrong/missing reader, public discovery, exact owned readiness, NUL and local-ID failures passed");
}
