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

auto embedded_items(std::vector<Item>& out) {
  return xq::Select("Embedded") | xq::Text() | xq::ParseXml() |
         xq::Select("Item") |
         xq::Construct<Item>(xq::Bind(&Item::id, xq::Attr("id"))) |
         xq::Invoke([&out](const Item& item) { out.push_back(item); });
}

TEST(XmlQueryTest, NestedDocumentIsParsed) {
  constexpr std::string_view kXml =
      R"(<root><Embedded>&lt;Item id="1"/&gt;</Embedded></root>)";
  std::vector<Item> items;
  xq::XmlParser parser(embedded_items(items)(xq::NullSink{}));

  EXPECT_EQ(parser.feed(kXml, true), xq::ParseStatus::Ok);
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items[0].id, "1");
}

TEST(XmlQueryTest, MalformedNestedDocumentFailsOuterParse) {
  // The outer document is well-formed; the embedded one has a mismatched tag.
  constexpr std::string_view kXml =
      R"(<root><Embedded>&lt;Item id="1"&gt;&lt;/Wrong&gt;</Embedded></root>)";
  std::vector<Item> items;
  xq::XmlParser parser(embedded_items(items)(xq::NullSink{}));

  EXPECT_EQ(parser.feed(kXml, true), xq::ParseStatus::Error);
}

TEST(XmlQueryTest, TruncatedNestedDocumentFailsOuterParse) {
  // Unclosed element: only detected when the nested document is finalized.
  constexpr std::string_view kXml =
      R"(<root><Embedded>&lt;Item id="1"&gt;</Embedded></root>)";
  std::vector<Item> items;
  xq::XmlParser parser(embedded_items(items)(xq::NullSink{}));

  EXPECT_EQ(parser.feed(kXml, true), xq::ParseStatus::Error);
}

}  // namespace
