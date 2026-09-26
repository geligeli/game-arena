#include "game_arena/server/job_log.h"

#include <unistd.h>

#include <filesystem>
#include <string>

#include "gtest/gtest.h"

namespace tournament_arena {
namespace {

class JobLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("job_log_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static JobRecord Record(const std::string &job_id, int64_t created_unix_ms,
                          proto::Job::State state = proto::Job::QUEUED) {
    JobRecord record;
    record.mutable_job()->set_job_id(job_id);
    record.mutable_job()->set_created_unix_ms(created_unix_ms);
    record.mutable_job()->set_state(state);
    return record;
  }

  std::filesystem::path dir_;
};

TEST_F(JobLogTest, KeepsEveryJobNewestFirst) {
  JobLog log(dir_);
  log.Put(Record("j1_1", 1000));
  log.Put(Record("j3_2", 3000));
  log.Put(Record("j2_3", 2000));

  const std::vector<JobRecord> records = log.List();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].job().job_id(), "j3_2");
  EXPECT_EQ(records[1].job().job_id(), "j2_3");
  EXPECT_EQ(records[2].job().job_id(), "j1_1");
  EXPECT_EQ(JobLog(dir_).Get("j2_3")->job().created_unix_ms(), 2000);
}

TEST_F(JobLogTest, PutReplacesTheRecord) {
  JobLog log(dir_);
  log.Put(Record("j1_1", 1000));
  log.Put(Record("j1_1", 1000, proto::Job::DONE));

  EXPECT_EQ(log.List().size(), 1u);
  EXPECT_EQ(log.Get("j1_1")->job().state(), proto::Job::DONE);
}

TEST_F(JobLogTest, RefusesIdsThatCannotNameAFile) {
  JobLog log(dir_ / "jobs");
  log.Put(Record("j1_1", 1000));
  JobRecord outside = Record("x", 1000);
  JobLog(dir_).Put(outside);

  EXPECT_FALSE(log.Get("../x").has_value());
  EXPECT_FALSE(log.Get("").has_value());
  EXPECT_FALSE(log.Get("unknown").has_value());
  EXPECT_TRUE(log.Get("j1_1").has_value());
}

}  // namespace
}  // namespace tournament_arena
