// Copyright © 2026 Nilton Volpato
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "xml_query/xml_query.h"

namespace {

struct Item {
  std::string id;
};

constexpr auto kEmbeddedItems =
    xq::Select("Embedded") | xq::Text() | xq::ParseXml() | xq::Select("Item") |
    xq::Construct<Item>(xq::Bind(&Item::id, xq::Attr("id")));

auto embedded_items_parser(std::vector<Item>& out) {
  return xq::XmlParser(kEmbeddedItems,
                       [&out](const Item& item) { out.push_back(item); });
}

TEST(XmlQueryTest, NestedDocumentIsParsed) {
  constexpr std::string_view kXml =
      R"(<root><Embedded>&lt;Item id="1"/&gt;</Embedded></root>)";
  std::vector<Item> items;
  auto parser = embedded_items_parser(items);

  EXPECT_EQ(parser.feed(kXml, true), xq::ParseStatus::Ok);
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items[0].id, "1");
}

TEST(XmlQueryTest, MalformedNestedDocumentFailsOuterParse) {
  // The outer document is well-formed; the embedded one has a mismatched tag.
  constexpr std::string_view kXml =
      R"(<root><Embedded>&lt;Item id="1"&gt;&lt;/Wrong&gt;</Embedded></root>)";
  std::vector<Item> items;
  auto parser = embedded_items_parser(items);

  EXPECT_EQ(parser.feed(kXml, true), xq::ParseStatus::Error);
}

TEST(XmlQueryTest, TruncatedNestedDocumentFailsOuterParse) {
  // Unclosed element: only detected when the nested document is finalized.
  constexpr std::string_view kXml =
      R"(<root><Embedded>&lt;Item id="1"&gt;</Embedded></root>)";
  std::vector<Item> items;
  auto parser = embedded_items_parser(items);

  EXPECT_EQ(parser.feed(kXml, true), xq::ParseStatus::Error);
}

// LastChange-shaped: the same element for several channels.
constexpr std::string_view kVolumes =
    R"(<Event><InstanceID val="0">)"
    R"(<Volume channel="Master" val="25"/><Volume channel="LF" val="100"/>)"
    R"(<Volume channel="RF" val="90"/></InstanceID></Event>)";

std::vector<std::string> volumes(auto select) {
  std::vector<std::string> out;
  xq::XmlParser parser(select | xq::Attr("val"), [&out](std::string_view v) {
    out.emplace_back(v);
  });
  EXPECT_EQ(parser.feed(kVolumes, true), xq::ParseStatus::Ok);
  return out;
}

TEST(XmlQueryTest, SelectWithAttributeValue) {
  EXPECT_EQ(volumes(xq::Select("Volume").with(xq::Attr("channel", "Master"))),
            std::vector<std::string>{"25"});
  EXPECT_EQ(volumes(xq::Select("Volume")),
            (std::vector<std::string>{"25", "100", "90"}));
}

TEST(XmlQueryTest, SelectWithPredicate) {
  auto front = xq::Predicate([](const xq::Attributes& attrs) {
    auto channel = attrs.get("channel");
    return channel == "LF" || channel == "RF";
  });
  EXPECT_EQ(volumes(xq::Select("Volume").with(front)),
            (std::vector<std::string>{"100", "90"}));
}

TEST(XmlQueryTest, ChainedWithMustAllMatch) {
  auto loud = xq::Predicate(
      [](const xq::Attributes& attrs) { return attrs.get("val") != "25"; });
  EXPECT_EQ(volumes(xq::Select("Volume")
                        .with(xq::Attr("channel", "RF"))
                        .with(loud)),
            std::vector<std::string>{"90"});
  EXPECT_TRUE(volumes(xq::Select("Volume")
                          .with(xq::Attr("channel", "Master"))
                          .with(loud))
                  .empty());
}

TEST(XmlQueryTest, FanoutRunsEveryBranch) {
  struct A {
    std::string id;
  };
  struct B {
    std::string id;
  };
  std::vector<std::string> seen;
  struct Record {
    std::vector<std::string>* seen;
    void operator()(const A& a) const { seen->push_back("a" + a.id); }
    void operator()(const B& b) const { seen->push_back("b" + b.id); }
  };
  constexpr auto kQuery = xq::Fanout(
      xq::Select("a") | xq::Construct<A>(xq::Bind(&A::id, xq::Attr("id"))),
      xq::Select("b") | xq::Construct<B>(xq::Bind(&B::id, xq::Attr("id"))));
  xq::XmlParser parser(kQuery, Record{&seen});
  ASSERT_EQ(parser.feed(R"(<r><a id="1"/><b id="2"/><a id="3"/></r>)", true),
            xq::ParseStatus::Ok);
  EXPECT_EQ(seen, (std::vector<std::string>{"a1", "b2", "a3"}));
}

}  // namespace
