#include <memory>
#include <vector>

#include "gtest/gtest.h"

#include "base_test.hpp"

#include "optimizer/abstract_syntax_tree/projection_node.hpp"
#include "optimizer/abstract_syntax_tree/stored_table_node.hpp"
#include "optimizer/expression.hpp"
#include "storage/storage_manager.hpp"

namespace opossum {

class ProjectionNodeTest : public BaseTest {
 protected:
  void SetUp() override {
    StorageManager::get().add_table("t_a", load_table("src/test/tables/int_int_int.tbl", 0));

    _stored_table_node = std::make_shared<StoredTableNode>("t_a");

    // SELECT c, a, b AS alias_for_b, b+c AS some_addition, a+c [...]
    _projection_node = std::make_shared<ProjectionNode>(std::vector<std::shared_ptr<Expression>>{
        Expression::create_column(ColumnID{2}), Expression::create_column(ColumnID{0}),
        Expression::create_column(ColumnID{1}, {"alias_for_b"}),
        Expression::create_binary_operator(ExpressionType::Addition, Expression::create_column(ColumnID{1}),
                                           Expression::create_column(ColumnID{2}), {"some_addition"}),
        Expression::create_binary_operator(ExpressionType::Addition, Expression::create_column(ColumnID{0}),
                                           Expression::create_column(ColumnID{2}))});
    _projection_node->set_left_child(_stored_table_node);
  }

  void TearDown() override { StorageManager::get().reset(); }

  std::shared_ptr<StoredTableNode> _stored_table_node;
  std::shared_ptr<ProjectionNode> _projection_node;
};

TEST_F(ProjectionNodeTest, ColumnIdForColumnIdentifier) {
  EXPECT_EQ(_projection_node->get_column_id_by_named_column_reference({"c", std::nullopt}), 0);
  EXPECT_EQ(_projection_node->get_column_id_by_named_column_reference({"c", {"t_a"}}), 0);
  EXPECT_EQ(_projection_node->get_column_id_by_named_column_reference({"a", std::nullopt}), 1);
  EXPECT_EQ(_projection_node->find_column_id_by_named_column_reference({"b", std::nullopt}), std::nullopt);
  EXPECT_EQ(_projection_node->find_column_id_by_named_column_reference({"b", {"t_a"}}), std::nullopt);
  EXPECT_EQ(_projection_node->get_column_id_by_named_column_reference({"alias_for_b", std::nullopt}), 2);
  EXPECT_EQ(_projection_node->find_column_id_by_named_column_reference({"alias_for_b", {"t_a"}}), std::nullopt);
  EXPECT_EQ(_projection_node->get_column_id_by_named_column_reference({"some_addition", std::nullopt}), 3);
  EXPECT_EQ(_projection_node->find_column_id_by_named_column_reference({"some_addition", {"t_a"}}), std::nullopt);
  EXPECT_EQ(_projection_node->find_column_id_by_named_column_reference({"some_addition", {"t_b"}}), std::nullopt);
  EXPECT_EQ(_projection_node->get_column_id_by_named_column_reference({"a + c", std::nullopt}), 4);
}

TEST_F(ProjectionNodeTest, AliasedSubqueryTest) {
  const auto projection_node_with_alias = std::make_shared<ProjectionNode>(*_projection_node);
  projection_node_with_alias->set_alias({"foo"});

  EXPECT_TRUE(projection_node_with_alias->knows_table("foo"));
  EXPECT_FALSE(projection_node_with_alias->knows_table("t_a"));

  EXPECT_EQ(projection_node_with_alias->get_column_id_by_named_column_reference({"c"}), ColumnID{0});
  EXPECT_EQ(projection_node_with_alias->get_column_id_by_named_column_reference({"c", {"foo"}}), ColumnID{0});
  EXPECT_EQ(projection_node_with_alias->find_column_id_by_named_column_reference({"c", {"t_a"}}), std::nullopt);
  EXPECT_EQ(projection_node_with_alias->find_column_id_by_named_column_reference({"a", {"t_b"}}), std::nullopt);
  EXPECT_EQ(projection_node_with_alias->find_column_id_by_named_column_reference({"b"}), std::nullopt);
  EXPECT_EQ(projection_node_with_alias->find_column_id_by_named_column_reference({"b", {"t_a"}}), std::nullopt);
  EXPECT_EQ(projection_node_with_alias->get_column_id_by_named_column_reference({"alias_for_b", std::nullopt}), 2);
  EXPECT_EQ(projection_node_with_alias->get_column_id_by_named_column_reference({"alias_for_b", {"foo"}}), 2);
  EXPECT_EQ(projection_node_with_alias->find_column_id_by_named_column_reference({"alias_for_b", {"t_a"}}),
            std::nullopt);
}

}  // namespace opossum
