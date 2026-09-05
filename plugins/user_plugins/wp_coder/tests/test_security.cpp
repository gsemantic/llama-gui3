#include "test_framework.h"
#include "../core/security.h"

using namespace coder::security;

TEST(security_path_safe_normal) {
    ASSERT_TRUE(is_path_safe("wp-content/themes/mytheme/style.css"));
}

TEST(security_path_safe_absolute) {
    ASSERT_TRUE(is_path_safe("/var/www/html/index.php"));
}

TEST(security_path_unsafe_traversal) {
    ASSERT_FALSE(is_path_safe("../../etc/passwd"));
}

TEST(security_path_unsafe_dotdot) {
    ASSERT_FALSE(is_path_safe("wp-content/../../../etc/shadow"));
}

TEST(security_path_not_dangerous_normal) {
    ASSERT_TRUE(is_path_not_dangerous("/var/www/html/wp-config.php"));
}

TEST(security_path_dangerous_etc) {
    ASSERT_FALSE(is_path_not_dangerous("/etc/passwd"));
}

TEST(security_path_dangerous_proc) {
    ASSERT_FALSE(is_path_not_dangerous("/proc/1/environ"));
}

TEST(security_command_allowed_safe) {
    ASSERT_TRUE(is_command_allowed("ls -la"));
    ASSERT_TRUE(is_command_allowed("git status"));
    ASSERT_TRUE(is_command_allowed("php -l file.php"));
}

TEST(security_command_blocked_rm) {
    ASSERT_FALSE(is_command_allowed("rm -rf /"));
}

TEST(security_command_blocked_mkfs) {
    ASSERT_FALSE(is_command_allowed("mkfs.ext4 /dev/sda"));
}

TEST(security_shell_escape_basic) {
    ASSERT_EQ(shell_escape("hello"), std::string("'hello'"));
}

TEST(security_shell_escape_quotes) {
    ASSERT_EQ(shell_escape("it's"), std::string("'it'\\''s'"));
}

TEST(security_shell_escape_empty) {
    ASSERT_EQ(shell_escape(""), std::string("''"));
}

TEST(security_project_dir_root) {
    ASSERT_FALSE(is_project_dir_valid("/"));
}

TEST(security_project_dir_etc) {
    ASSERT_FALSE(is_project_dir_valid("/etc"));
}

TEST(security_project_dir_usr) {
    ASSERT_FALSE(is_project_dir_valid("/usr"));
}

TEST(security_project_dir_var) {
    ASSERT_TRUE(is_project_dir_valid("/var/www/mysite"));
}

TEST(security_project_dir_valid) {
    ASSERT_TRUE(is_project_dir_valid("/home/user/project"));
    ASSERT_TRUE(is_project_dir_valid("/var/www/mysite"));
    ASSERT_TRUE(is_project_dir_valid("/tmp/test"));
    ASSERT_TRUE(is_project_dir_valid("/opt/myapp"));
}

TEST(security_project_dir_empty) {
    ASSERT_TRUE(is_project_dir_valid(""));
}
