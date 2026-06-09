#include "../include/mid/dom.hpp"
#include <cassert>
#include <iostream>

using namespace exodus::mid_ir;

auto test_diamond() -> void {
  // Entry -> A, B
  // A -> Exit
  // B -> Exit
  LinearFunction func;
  auto entry = std::make_unique<Block>("entry");
  auto a = std::make_unique<Block>("A");
  auto b = std::make_unique<Block>("B");
  auto exit = std::make_unique<Block>("exit");

  Block *p_entry = entry.get();
  Block *p_a = a.get();
  Block *p_b = b.get();
  Block *p_exit = exit.get();

  p_entry->succs = {p_a, p_b};
  p_a->preds = {p_entry};
  p_a->succs = {p_exit};
  p_b->preds = {p_entry};
  p_b->succs = {p_exit};
  p_exit->preds = {p_a, p_b};

  func.blocks.push_back(std::move(entry));
  func.blocks.push_back(std::move(a));
  func.blocks.push_back(std::move(b));
  func.blocks.push_back(std::move(exit));

  DomTree dt;
  dt.compute(func);

  assert(dt.get_idom(p_entry) == nullptr);
  assert(dt.get_idom(p_a) == p_entry);
  assert(dt.get_idom(p_b) == p_entry);
  assert(dt.get_idom(p_exit) == p_entry);

  // DF(entry) = {}
  // DF(A) = {exit}
  // DF(B) = {exit}
  // DF(exit) = {}
  assert(dt.get_df(p_entry).empty());
  assert(dt.get_df(p_a).size() == 1 && dt.get_df(p_a)[0] == p_exit);
  assert(dt.get_df(p_b).size() == 1 && dt.get_df(p_b)[0] == p_exit);
  assert(dt.get_df(p_exit).empty());

  assert(dt.dominate(p_entry, p_a));
  assert(dt.dominate(p_entry, p_b));
  assert(dt.dominate(p_entry, p_exit));
  assert(!dt.dominate(p_a, p_exit));
  assert(!dt.dominate(p_b, p_exit));

  std::cout << "test_diamond passed!\n";
}

auto test_loop() -> void {
  // Entry -> Header
  // Header -> Body, Exit
  // Body -> Header
  LinearFunction func;
  auto entry = std::make_unique<Block>("entry");
  auto header = std::make_unique<Block>("header");
  auto body = std::make_unique<Block>("body");
  auto exit = std::make_unique<Block>("exit");

  Block *p_entry = entry.get();
  Block *p_header = header.get();
  Block *p_body = body.get();
  Block *p_exit = exit.get();

  p_entry->succs = {p_header};
  p_header->preds = {p_entry, p_body};
  p_header->succs = {p_body, p_exit};
  p_body->preds = {p_header};
  p_body->succs = {p_header};
  p_exit->preds = {p_header};

  func.blocks.push_back(std::move(entry));
  func.blocks.push_back(std::move(header));
  func.blocks.push_back(std::move(body));
  func.blocks.push_back(std::move(exit));

  DomTree dt;
  dt.compute(func);

  assert(dt.get_idom(p_header) == p_entry);
  assert(dt.get_idom(p_body) == p_header);
  assert(dt.get_idom(p_exit) == p_header);

  // DF(header) = {header} (because of backedge body->header)
  // DF(body) = {header}
  assert(dt.get_df(p_header).size() == 1 && dt.get_df(p_header)[0] == p_header);
  assert(dt.get_df(p_body).size() == 1 && dt.get_df(p_body)[0] == p_header);
  assert(dt.get_df(p_exit).empty());

  std::cout << "test_loop passed!\n";
}

auto test_unreachable() -> void {
  // Entry -> A
  // Unreachable -> B
  LinearFunction func;
  auto entry = std::make_unique<Block>("entry");
  auto a = std::make_unique<Block>("A");
  auto unreachable = std::make_unique<Block>("unreachable");
  auto b = std::make_unique<Block>("B");

  Block *p_entry = entry.get();
  Block *p_a = a.get();
  Block *p_unreachable = unreachable.get();
  Block *p_b = b.get();

  p_entry->succs = {p_a};
  p_a->preds = {p_entry};

  p_unreachable->succs = {p_b};
  p_b->preds = {p_unreachable};

  func.blocks.push_back(std::move(entry));
  func.blocks.push_back(std::move(a));
  func.blocks.push_back(std::move(unreachable));
  func.blocks.push_back(std::move(b));

  DomTree dt;
  dt.compute(func);

  assert(dt.get_idom(p_a) == p_entry);
  assert(dt.get_idom(p_unreachable) == nullptr);
  assert(dt.get_idom(p_b) == nullptr);

  assert(dt.dominate(p_entry, p_a));
  assert(!dt.dominate(p_entry, p_unreachable));

  std::cout << "test_unreachable passed!\n";
}

#ifdef EXODUS_UNIT_TEST
auto main() -> int {
  test_diamond();
  test_loop();
  test_unreachable();
  return 0;
}
#endif
