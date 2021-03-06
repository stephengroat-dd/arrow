// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/dataset/discovery.h"

#include <memory>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "arrow/dataset/partition.h"
#include "arrow/dataset/test_util.h"
#include "arrow/filesystem/test_util.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/type_fwd.h"

namespace arrow {
namespace dataset {

void AssertSchemasAre(std::vector<std::shared_ptr<Schema>> actual,
                      std::vector<std::shared_ptr<Schema>> expected) {
  EXPECT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); i++) {
    EXPECT_EQ(*actual[i], *expected[i]);
  }
}

class DatasetFactoryTest : public TestFileSystemDataset {
 public:
  void AssertInspect(const std::vector<std::shared_ptr<Field>>& expected_fields) {
    ASSERT_OK_AND_ASSIGN(auto actual, factory_->Inspect());
    EXPECT_EQ(*actual, Schema(expected_fields));
  }

  void AssertInspectSchemas(std::vector<std::shared_ptr<Schema>> expected) {
    ASSERT_OK_AND_ASSIGN(auto actual, factory_->InspectSchemas());
    AssertSchemasAre(actual, expected);
  }

 protected:
  std::shared_ptr<DatasetFactory> factory_;
};

class MockDatasetFactory : public DatasetFactory {
 public:
  explicit MockDatasetFactory(std::vector<std::shared_ptr<Schema>> schemas)
      : schemas_(std::move(schemas)) {}

  Result<std::vector<std::shared_ptr<Schema>>> InspectSchemas() override {
    return schemas_;
  }

  Result<std::shared_ptr<Dataset>> Finish(
      const std::shared_ptr<Schema>& schema) override {
    return std::make_shared<InMemoryDataset>(schema,
                                             std::vector<std::shared_ptr<RecordBatch>>{});
  }

 protected:
  std::vector<std::shared_ptr<Schema>> schemas_;
};

class MockPartitioning : public Partitioning {
 public:
  explicit MockPartitioning(std::shared_ptr<Schema> schema)
      : Partitioning(std::move(schema)) {}

  Result<std::shared_ptr<Expression>> Parse(const std::string& segment,
                                            int i) const override {
    return nullptr;
  }

  std::string type_name() const override { return "mock_partitioning"; }
};

class MockDatasetFactoryTest : public DatasetFactoryTest {
 public:
  void MakeFactory(std::vector<std::shared_ptr<Schema>> schemas) {
    factory_ = std::make_shared<MockDatasetFactory>(schemas);
  }

 protected:
  std::shared_ptr<Field> i32 = field("i32", int32());
  std::shared_ptr<Field> i64 = field("i64", int64());
  std::shared_ptr<Field> f32 = field("f32", float64());
  std::shared_ptr<Field> f64 = field("f64", float64());
  // Non-nullable
  std::shared_ptr<Field> i32_req = field("i32", int32(), false);
  // bad type with name `i32`
  std::shared_ptr<Field> i32_fake = field("i32", boolean());
};

TEST_F(MockDatasetFactoryTest, UnifySchemas) {
  MakeFactory({});
  AssertInspect({});

  MakeFactory({schema({i32}), schema({i32})});
  AssertInspect({i32});

  MakeFactory({schema({i32}), schema({i64})});
  AssertInspect({i32, i64});

  MakeFactory({schema({i32}), schema({i64})});
  AssertInspect({i32, i64});

  MakeFactory({schema({i32}), schema({i32_req})});
  AssertInspect({i32});

  MakeFactory({schema({i32, f64}), schema({i32_req, i64})});
  AssertInspect({i32, f64, i64});

  MakeFactory({schema({i32, f64}), schema({f64, i32_fake})});
  // Unification fails when fields with the same name have clashing types.
  ASSERT_RAISES(Invalid, factory_->Inspect());
  // Return the individual schema for closer inspection should not fail.
  AssertInspectSchemas({schema({i32, f64}), schema({f64, i32_fake})});
}

class FileSystemDatasetFactoryTest : public DatasetFactoryTest {
 public:
  void MakeFactory(const std::vector<fs::FileInfo>& files) {
    MakeFileSystem(files);
    ASSERT_OK_AND_ASSIGN(factory_, FileSystemDatasetFactory::Make(fs_, selector_, format_,
                                                                  factory_options_));
  }

  void AssertFinishWithPaths(std::vector<std::string> paths,
                             std::shared_ptr<Schema> schema = nullptr) {
    if (schema == nullptr) {
      ASSERT_OK_AND_ASSIGN(schema, factory_->Inspect());
    }
    options_ = ScanOptions::Make(schema);
    ASSERT_OK_AND_ASSIGN(source_, factory_->Finish(schema));
    AssertFragmentsAreFromPath(source_->GetFragments(options_), paths);
  }

 protected:
  fs::FileSelector selector_;
  FileSystemFactoryOptions factory_options_;
  std::shared_ptr<FileFormat> format_ = std::make_shared<DummyFileFormat>(schema({}));
};

TEST_F(FileSystemDatasetFactoryTest, Basic) {
  MakeFactory({fs::File("a"), fs::File("b")});
  AssertFinishWithPaths({"a", "b"});
  MakeFactory({fs::Dir("a"), fs::Dir("a/b"), fs::File("a/b/c")});
}

TEST_F(FileSystemDatasetFactoryTest, Selector) {
  selector_.base_dir = "A";
  selector_.recursive = true;

  MakeFactory({fs::File("0"), fs::File("A/a"), fs::File("A/A/a")});
  // "0" doesn't match selector, so it has been dropped:
  AssertFinishWithPaths({"A/a", "A/A/a"});

  factory_options_.partition_base_dir = "A/A";
  MakeFactory({fs::File("0"), fs::File("A/a"), fs::File("A/A/a")});
  // partition_base_dir should not affect filtered files, only the applied partition
  AssertInspect({});
  AssertFinishWithPaths({"A/a", "A/A/a"});
}

TEST_F(FileSystemDatasetFactoryTest, ExplicitPartition) {
  selector_.base_dir = "a=ignored/base";
  factory_options_.partitioning =
      std::make_shared<HivePartitioning>(schema({field("a", float64())}));

  MakeFactory(
      {fs::File(selector_.base_dir + "/a=1"), fs::File(selector_.base_dir + "/a=2")});

  AssertInspect({field("a", float64())});
  AssertFinishWithPaths({selector_.base_dir + "/a=1", selector_.base_dir + "/a=2"});
}

TEST_F(FileSystemDatasetFactoryTest, DiscoveredPartition) {
  selector_.base_dir = "a=ignored/base";
  factory_options_.partitioning = HivePartitioning::MakeFactory();
  MakeFactory(
      {fs::File(selector_.base_dir + "/a=1"), fs::File(selector_.base_dir + "/a=2")});

  AssertInspect({field("a", int32())});
  AssertFinishWithPaths({selector_.base_dir + "/a=1", selector_.base_dir + "/a=2"});
}

TEST_F(FileSystemDatasetFactoryTest, MissingDirectories) {
  MakeFileSystem({fs::File("base_dir/a=3/b=3/dat"), fs::File("unpartitioned/ignored=3")});

  factory_options_.partition_base_dir = "base_dir";
  factory_options_.partitioning = std::make_shared<HivePartitioning>(
      schema({field("a", int32()), field("b", int32())}));

  ASSERT_OK_AND_ASSIGN(
      factory_, FileSystemDatasetFactory::Make(
                    fs_, {"base_dir/a=3/b=3/dat", "unpartitioned/ignored=3"}, format_,
                    factory_options_));

  AssertInspect({field("a", int32()), field("b", int32())});
  AssertFinishWithPaths({"base_dir/a=3/b=3/dat", "unpartitioned/ignored=3"});
}

TEST_F(FileSystemDatasetFactoryTest, OptionsIgnoredDefaultPrefixes) {
  MakeFactory({
      fs::File("."),
      fs::File("_"),
      fs::File("_$folder$"),
      fs::File("_SUCCESS"),
      fs::File("not_ignored_by_default"),
  });

  AssertFinishWithPaths({"not_ignored_by_default"});
}

TEST_F(FileSystemDatasetFactoryTest, OptionsIgnoredCustomPrefixes) {
  factory_options_.ignore_prefixes = {"not_ignored"};
  MakeFactory({
      fs::File("."),
      fs::File("_"),
      fs::File("_$folder$"),
      fs::File("_SUCCESS"),
      fs::File("not_ignored_by_default"),
  });

  AssertFinishWithPaths({".", "_", "_$folder$", "_SUCCESS"});
}

TEST_F(FileSystemDatasetFactoryTest, Inspect) {
  auto s = schema({field("f64", float64())});
  format_ = std::make_shared<DummyFileFormat>(s);

  // No files
  MakeFactory({});
  AssertInspect({});

  MakeFactory({fs::File("test")});
  AssertInspect(s->fields());
}

TEST_F(FileSystemDatasetFactoryTest, FinishWithIncompatibleSchemaShouldFail) {
  auto s = schema({field("f64", float64())});
  format_ = std::make_shared<DummyFileFormat>(s);

  auto broken_s = schema({field("f64", utf8())});
  // No files
  MakeFactory({});
  ASSERT_OK_AND_ASSIGN(auto dataset, factory_->Finish(broken_s));

  MakeFactory({fs::File("test")});
  ASSERT_RAISES(Invalid, factory_->Finish(broken_s));
}

std::shared_ptr<DatasetFactory> DatasetFactoryFromSchemas(
    std::vector<std::shared_ptr<Schema>> schemas) {
  return std::make_shared<MockDatasetFactory>(schemas);
}

TEST(UnionDatasetFactoryTest, Basic) {
  auto f64 = field("f64", float64());
  auto i32 = field("i32", int32());
  auto i32_req = field("i32", int32(), /*nullable*/ false);
  auto str = field("str", utf8());

  auto schema_1 = schema({f64, i32_req});
  auto schema_2 = schema({f64, i32});
  auto schema_3 = schema({str, i32});

  auto source_1 = DatasetFactoryFromSchemas({schema_1, schema_2});
  auto source_2 = DatasetFactoryFromSchemas({schema_2});
  auto source_3 = DatasetFactoryFromSchemas({schema_3});

  ASSERT_OK_AND_ASSIGN(auto factory,
                       UnionDatasetFactory::Make({source_1, source_2, source_3}));

  ASSERT_OK_AND_ASSIGN(auto schemas, factory->InspectSchemas());
  AssertSchemasAre(schemas, {schema_2, schema_2, schema_3});

  auto expected_schema = schema({f64, i32, str});
  ASSERT_OK_AND_ASSIGN(auto inspected, factory->Inspect());
  EXPECT_EQ(*inspected, *expected_schema);

  ASSERT_OK_AND_ASSIGN(auto dataset, factory->Finish());
  EXPECT_EQ(*dataset->schema(), *expected_schema);

  auto f64_schema = schema({f64});
  ASSERT_OK_AND_ASSIGN(dataset, factory->Finish(f64_schema));
  EXPECT_EQ(*dataset->schema(), *f64_schema);
}

TEST(UnionDatasetFactoryTest, ConflictingSchemas) {
  auto f64 = field("f64", float64());
  auto i32 = field("i32", int32());
  auto i32_req = field("i32", int32(), /*nullable*/ false);
  auto bad_f64 = field("f64", float32());

  auto schema_1 = schema({f64, i32_req});
  auto schema_2 = schema({f64, i32});
  // Incompatible with schema_1
  auto schema_3 = schema({bad_f64, i32});

  auto source_factory_1 = DatasetFactoryFromSchemas({schema_1, schema_2});
  auto source_factory_2 = DatasetFactoryFromSchemas({schema_2});
  auto source_factory_3 = DatasetFactoryFromSchemas({schema_3});

  ASSERT_OK_AND_ASSIGN(
      auto factory,
      UnionDatasetFactory::Make({source_factory_1, source_factory_2, source_factory_3}));

  // schema_3 conflicts with other, Inspect/Finish should not work
  ASSERT_RAISES(Invalid, factory->Inspect());
  ASSERT_RAISES(Invalid, factory->Finish());

  // The user can inspect without error
  ASSERT_OK_AND_ASSIGN(auto schemas, factory->InspectSchemas());
  AssertSchemasAre(schemas, {schema_2, schema_2, schema_3});

  // The user decided to ignore the conflicting `f64` field.
  auto i32_schema = schema({i32});
  ASSERT_OK_AND_ASSIGN(auto dataset, factory->Finish(i32_schema));
  EXPECT_EQ(*dataset->schema(), *i32_schema);
}

}  // namespace dataset
}  // namespace arrow
