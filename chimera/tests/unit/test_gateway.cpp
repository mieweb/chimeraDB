// The two halves of the cross-language gateways that can be tested without a
// server: what counts as a read-only SQL statement, and how a statement pasted
// out of a mongo shell is understood.
#include <doctest/doctest.h>

#include <string>

#include "chimera/codec.h"
#include "chimera/error.h"
#include "chimera/mongosh.h"
#include "chimera/sqlguard.h"

using namespace chimera;

namespace {

// Arguments are compared as extended JSON, which is also how they will be
// spelled when the gateway hands them to a command handler.
std::string arg_text(const ShellCall& call, size_t index) {
  REQUIRE(call.args.size() > index);
  const bson_value_t& value = call.args[index].get();
  bson_t view;
  REQUIRE(bson_init_static(&view, value.value.v_doc.data, value.value.v_doc.data_len));
  return to_extjson(&view);
}

std::string command_text(const std::string& statement) {
  return to_extjson(build_shell_command(parse_shell_call(statement)).command.get());
}

}  // namespace

TEST_CASE("a read-only statement is recognised through comments and casing") {
  CHECK(is_read_only_statement("select 1"));
  CHECK(is_read_only_statement("  \n\t SELECT * FROM t"));
  CHECK(is_read_only_statement("/* a note */ SELECT 1"));
  CHECK(is_read_only_statement("-- a note\nSHOW TABLES"));
  CHECK(is_read_only_statement("# a note\nEXPLAIN SELECT 1"));
  CHECK(leading_keyword("/*x*/ /*y*/ describe t") == "DESCRIBE");
}

TEST_CASE("anything that is not plainly a read is refused") {
  CHECK_FALSE(is_read_only_statement("DELETE FROM t"));
  CHECK_FALSE(is_read_only_statement("  UPDATE t SET a = 1"));
  CHECK_FALSE(is_read_only_statement("DROP TABLE t"));
  CHECK_FALSE(is_read_only_statement("CALL p()"));
  CHECK_FALSE(is_read_only_statement(""));
  // A CTE is a read right up until the statement it introduces is a DELETE, so
  // the first keyword stops being evidence and the whole form is refused.
  CHECK_FALSE(is_read_only_statement("WITH x AS (SELECT 1) SELECT * FROM x"));
  // An unterminated comment must not leave a keyword visible behind it.
  CHECK_FALSE(is_read_only_statement("/* SELECT"));
}

TEST_CASE("only IPv4 loopback addresses count as loopback") {
  CHECK(is_loopback_address("127.0.0.1"));
  CHECK(is_loopback_address("127.0.0.53"));  // the whole /8 is loopback
  CHECK(is_loopback_address("127.255.255.255"));
  CHECK_FALSE(is_loopback_address("0.0.0.0"));
  CHECK_FALSE(is_loopback_address("10.0.0.1"));
  CHECK_FALSE(is_loopback_address("128.0.0.1"));
  CHECK_FALSE(is_loopback_address("localhost"));  // the listener is IPv4-only
  CHECK_FALSE(is_loopback_address("::1"));
  CHECK_FALSE(is_loopback_address(""));
  // An embedded NUL must not hide the tail of the string from the parser.
  CHECK_FALSE(is_loopback_address(std::string_view("127.0.0.1\0evil", 14)));
}

TEST_CASE("a pasted find survives unquoted keys and single quotes") {
  const ShellCall call = parse_shell_call("db.users.find({name: 'ada', active: true})");
  CHECK(call.collection == "users");
  CHECK(call.verb == "find");
  CHECK(call.args.size() == 1);
  CHECK(arg_text(call, 0) == R"({ "name" : "ada", "active" : true })");
}

TEST_CASE("integers stay integers and reals stay reals") {
  const ShellCall call = parse_shell_call("db.parts.insertOne({qty: 3, mass: 1.5, tag: null})");
  CHECK(arg_text(call, 0) ==
        R"({ "qty" : { "$numberLong" : "3" }, "mass" : { "$numberDouble" : "1.5" }, )"
        R"("tag" : null })");
}

TEST_CASE("a second argument and nested shapes come through in order") {
  const ShellCall call =
      parse_shell_call("db.users.find({age: {$gt: 30}}, {projection: {name: 1, _id: 0}})");
  CHECK(call.args.size() == 2);
  CHECK(arg_text(call, 0) == R"({ "age" : { "$gt" : { "$numberLong" : "30" } } })");
  CHECK(arg_text(call, 1) ==
        R"({ "projection" : { "name" : { "$numberLong" : "1" }, )"
        R"("_id" : { "$numberLong" : "0" } } })");
}

TEST_CASE("an aggregation pipeline is an array argument") {
  const ShellCall call =
      parse_shell_call("db.orders.aggregate([{$match: {paid: true}}, {$count: 'n'}])");
  CHECK(call.verb == "aggregate");
  CHECK(arg_text(call, 0) ==
        R"({ "0" : { "$match" : { "paid" : true } }, "1" : { "$count" : "n" } })");
}

TEST_CASE("ObjectId is a value, because every real paste contains one") {
  const ShellCall call =
      parse_shell_call("db.users.deleteOne({_id: ObjectId('64b7f0c2a1b2c3d4e5f60718')})");
  CHECK(arg_text(call, 0) == R"({ "_id" : { "$oid" : "64b7f0c2a1b2c3d4e5f60718" } })");
}

TEST_CASE("no arguments, and a trailing semicolon, are both fine") {
  const ShellCall call = parse_shell_call("  db.users.countDocuments();  ");
  CHECK(call.verb == "countDocuments");
  CHECK(call.args.empty());
}

TEST_CASE("malformed and unsupported statements fail rather than guess") {
  CHECK_THROWS_AS(parse_shell_call("users.find({})"), TranslatorError);
  CHECK_THROWS_AS(parse_shell_call("db.users.find({}"), TranslatorError);
  CHECK_THROWS_AS(parse_shell_call("db.users.find({}) db.users.drop()"), TranslatorError);
  CHECK_THROWS_AS(parse_shell_call("db.users.find({a: 'unterminated})"), TranslatorError);
  // A shell helper we cannot evaluate is an error, never a silently dropped
  // argument.
  CHECK_THROWS_AS(parse_shell_call("db.users.find({at: ISODate('2020-01-01')})"),
                  TranslatorError);
}

TEST_CASE("every verb becomes the command a driver would have sent") {
  CHECK(command_text("db.users.find()") == R"({ "find" : "users", "filter" : { } })");
  CHECK(command_text("db.users.findOne({a: 1}, {b: 1})") ==
        R"({ "find" : "users", "filter" : { "a" : { "$numberLong" : "1" } }, )"
        R"("projection" : { "b" : { "$numberLong" : "1" } }, "limit" : { "$numberLong" : "1" } })");
  CHECK(command_text("db.users.insertOne({_id: 'a'})") ==
        R"({ "insert" : "users", "documents" : [ { "_id" : "a" } ] })");
  CHECK(command_text("db.users.insertMany([{_id: 'a'}, {_id: 'b'}])") ==
        R"({ "insert" : "users", "documents" : [ { "_id" : "a" }, { "_id" : "b" } ] })");
  CHECK(command_text("db.users.updateMany({a: 1}, {$set: {b: 2}})") ==
        R"({ "update" : "users", "updates" : [ { "q" : { "a" : { "$numberLong" : "1" } }, )"
        R"("u" : { "$set" : { "b" : { "$numberLong" : "2" } } }, "multi" : true } ] })");
  CHECK(command_text("db.users.deleteOne({a: 1})") ==
        R"({ "delete" : "users", "deletes" : [ { "q" : { "a" : { "$numberLong" : "1" } }, )"
        R"("limit" : { "$numberInt" : "1" } } ] })");
  CHECK(command_text("db.users.deleteMany({})") ==
        R"({ "delete" : "users", "deletes" : [ { "q" : { }, "limit" : { "$numberInt" : "0" } } ] })");
  CHECK(command_text("db.users.countDocuments({a: 1})") ==
        R"({ "count" : "users", "query" : { "a" : { "$numberLong" : "1" } } })");
  CHECK(command_text("db.users.aggregate([{$count: 'n'}])") ==
        R"({ "aggregate" : "users", "pipeline" : [ { "$count" : "n" } ], "cursor" : { } })");
}

TEST_CASE("the reply a verb expects is part of the translation") {
  CHECK(build_shell_command(parse_shell_call("db.u.find()")).result == ShellResult::Documents);
  CHECK(build_shell_command(parse_shell_call("db.u.findOne()")).result ==
        ShellResult::FirstDocument);
  CHECK(build_shell_command(parse_shell_call("db.u.countDocuments()")).result ==
        ShellResult::Count);
  CHECK(build_shell_command(parse_shell_call("db.u.insertOne({})")).result ==
        ShellResult::Reply);
}

TEST_CASE("a verb outside the supported set is refused by name") {
  CHECK_THROWS_AS(build_shell_command(parse_shell_call("db.u.drop()")), TranslatorError);
  CHECK_THROWS_AS(build_shell_command(parse_shell_call("db.u.updateOne({a: 1})")),
                  TranslatorError);
  CHECK_THROWS_AS(build_shell_command(parse_shell_call("db.u.aggregate({})")), TranslatorError);
  CHECK_THROWS_AS(build_shell_command(parse_shell_call("db.u.find({}, {}, {})")),
                  TranslatorError);
}
