#include <doctest/doctest.h>
#include <algorithm>
#include "scram.h"
#include "cursor.h"

using namespace chimera;
namespace {
ScramCredential vector_credential() {
  ScramCredential c;
  c.user = "user";
  c.salt = base64_decode("W22ZaJ0SNY7soEsUEjb6gQ==");
  c.iterations = 4096;
  const auto stored = hex_decode("586e5df283e6dceb5c3e791d8b8528ec191e664045ce971792e2e6b5bb13e2a6");
  const auto server = hex_decode("c1f3cbc1c13a9d35a14c0990eed97629ea225863e566a4314ab99f3f00e5d9d5");
  std::copy(stored.begin(), stored.end(), c.stored_key.begin());
  std::copy(server.begin(), server.end(), c.server_key.begin());
  return c;
}
const std::string first = "n,,n=user,r=rOprNGfwEbeRWgbNEkqO";
const std::string suffix = "%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";
const std::string final_message = "c=biws,r=rOprNGfwEbeRWgbNEkqO" + suffix +
                                ",p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
}
TEST_CASE("SCRAM SHA256 matches the published RFC 7677 exchange") {
  ScramConversation c;
  CHECK(c.start(first, vector_credential(), true, suffix) ==
        "r=rOprNGfwEbeRWgbNEkqO" + suffix + ",s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096");
  CHECK(c.finish(final_message) == "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=");
  CHECK_THROWS(c.finish(final_message));
}
TEST_CASE("SCRAM rejects unknown users even when proof matches dummy keys") {
  ScramConversation c;
  c.start(first, vector_credential(), false, suffix);
  CHECK_THROWS(c.finish(final_message));
}
TEST_CASE("SCRAM rejects malformed first messages and identities") {
  for (const std::string bad : {"", "p=tls-unique,,n=user,r=long-enough-nonce", "n,a=admin,n=user,r=long-enough-nonce",
        "n,,n=user,r=short", "n,,n=user,r=long-enough-nonce,r=duplicate", "n,,n=user,m=required,r=long-enough-nonce",
        "n,,n=us=00er,r=long-enough-nonce", "n,,n=user,r=long-enough-nonce,"}) {
    ScramConversation c;
    CHECK_THROWS(c.start(bad, vector_credential(), true));
  }
  CHECK(ScramConversation::username("n,,n=a=2Cb=3Dc,r=long-enough-nonce") == "a,b=c");
}
TEST_CASE("SCRAM rejects proof, nonce and channel-binding substitutions") {
  for (const std::string bad : std::vector<std::string>{
       "c=biws,r=incorrect-nonce,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
       "c=eSws,r=rOprNGfwEbeRWgbNEkqO" + suffix + ",p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
       "c=biws,r=rOprNGfwEbeRWgbNEkqO" + suffix + ",p=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
       final_message + ",p=duplicate", final_message + ",x=trailing"}) {
    ScramConversation c;
    c.start(first, vector_credential(), true, suffix);
    CHECK_THROWS(c.finish(bad));
    CHECK_THROWS(c.finish(final_message));
  }
  ScramConversation c;
  CHECK_THROWS(c.finish(final_message));
}
TEST_CASE("SCRAM strictly validates encoding, iteration limits and conversation reset") {
  for (const auto* bad : {"=", "====", "a===", "YWJj\n", "YWJ=", "a!bc"})
    CHECK_THROWS(base64_decode(bad));
  CHECK(base64_decode(base64_encode(std::string("a\0b", 3))) == std::string("a\0b", 3));
  for (int iterations : {0, 4095, 1000001}) {
    auto credential = vector_credential(); credential.iterations = iterations;
    ScramConversation c;
    CHECK_THROWS(c.start(first, credential, true));
  }
  ScramConversation c;
  c.start(first, vector_credential(), true, suffix);
  c.reset();
  CHECK_THROWS(c.finish(final_message));
}
TEST_CASE("materialized and tail cursors are owned by an authenticated principal") {
  auto& cursors = CursorRegistry::instance();
  std::vector<Bson> docs; docs.emplace_back(); docs.emplace_back();
  const auto id = cursors.open("db.items", std::move(docs), "alice");
  bool exhausted = false;
  CHECK_THROWS(cursors.next_batch(id, "db.items", 1, &exhausted, "bob"));
  CHECK_FALSE(cursors.kill(id, "bob", "db.items"));
  CHECK_FALSE(cursors.kill(id, "alice", "other.items"));
  CHECK(cursors.next_batch(id, "db.items", 1, &exhausted, "alice").size() == 1);
  CHECK(cursors.kill(id, "alice", "db.items"));
  const auto tail_id = cursors.open_tail("db.items", TailState{}, "alice");
  TailState tail;
  CHECK_FALSE(cursors.tail_state(tail_id, "db.items", &tail, "bob"));
  cursors.advance_tail(tail_id, 100, "bob");
  CHECK(cursors.tail_state(tail_id, "db.items", &tail, "alice"));
  CHECK(tail.after_seq == 0);
  CHECK(cursors.kill(tail_id, "alice", "db.items"));
}
