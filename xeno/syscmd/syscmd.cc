#include<algorithm>
#include<cerrno>
#include<cstdio>
#include<cstring>
#include<string>
#include<string_view>
#include<utility>
#include<vector>

#include<dirent.h>
#include<fcntl.h>
#include<sys/stat.h>

#include<xeno/argsparser.h>
#include<xeno/filesystem.h>
#include<xeno/shell.h>
#include<xeno/syscmd.h>

namespace
{

using xeno::argsparser;

// syscmd 维护一个用户态当前目录；所有路径在进入 VFS 前都转成绝对路径。
std::string cwd = "/";

std::string absolute(std::string_view path)
{
    if(!path.empty() && path.front() == '/')
        return std::string{xeno::path{path}};

    std::string s = cwd;
    if(s != "/")
        s.push_back('/');
    s.append(path);
    return std::string{xeno::path{s}};
}

std::string join(const std::string &dir, std::string_view name)
{
    std::string s = dir;
    if(s != "/")
        s.push_back('/');
    s.append(name);
    return std::string{xeno::path{s}};
}

void report(const char *cmd, std::string_view path)
{
    std::printf("%s: %.*s: %s\n", cmd, (int)path.size(), path.data(), std::strerror(errno));
}

// 0=继续执行，1=已处理（help），-1=解析错误
int parse_args(argsparser &parser, int argc, const char *argv[])
{
    switch(parser.parse(argc, argv))
    {
    case argsparser::status::ok:
        return 0;
    case argsparser::status::help:
        return 1;
    case argsparser::status::error:
        parser.print_error();
        return -1;
    }
    return -1;
}

std::string mode_string(mode_t mode)
{
    std::string s = "----------";
    if(S_ISDIR(mode)) s[0] = 'd';
    else if(S_ISCHR(mode)) s[0] = 'c';
    else if(S_ISBLK(mode)) s[0] = 'b';
    else if(S_ISFIFO(mode)) s[0] = 'p';
    else if(S_ISLNK(mode)) s[0] = 'l';
    else if(S_ISSOCK(mode)) s[0] = 's';

    constexpr char rwx[] = "rwxrwxrwx";
    for(int i = 0; i < 9; i++)
        if(mode & (0400 >> i))
            s[i + 1] = rwx[i];
    return s;
}

int cmd_ls(int argc, const char *argv[])
{
    bool long_format = false;
    bool show_all = false;
    std::vector<std::string_view> paths;

    argsparser parser;
    parser.add_argument("-l", "--long").flag().help("long listing")
        .action([&]{ long_format = true; });
    parser.add_argument("-a", "--all").flag().help("include entries starting with .")
        .action([&]{ show_all = true; });
    parser.add_argument("path").any_args().help("files or directories")
        .action([&](std::string_view v){ paths.push_back(v); });

    int st = parse_args(parser, argc, argv);
    if(st != 0)
        return st < 0 ? 2 : 0;
    if(paths.empty())
        paths.push_back(".");

    bool multi = paths.size() > 1;
    int rc = 0;

    for(auto v: paths)
    {
        std::string ap = absolute(v);
        struct stat sb;
        if(__vfs.stat(ap.c_str(), &sb) < 0)
        {
            report("ls", ap);
            rc = 1;
            continue;
        }

        if(!S_ISDIR(sb.st_mode))
        {
            if(long_format)
                std::printf("%s %8ld %.*s\n", mode_string(sb.st_mode).c_str(),
                            (long)sb.st_size, (int)v.size(), v.data());
            else
                std::printf("%.*s\n", (int)v.size(), v.data());
            continue;
        }

        DIR *d = opendir(ap.c_str());
        if(d == nullptr)
        {
            report("ls", ap);
            rc = 1;
            continue;
        }
        if(multi)
            std::printf("%.*s:\n", (int)v.size(), v.data());

        std::vector<std::string> names;
        struct dirent *e;
        while((e = readdir(d)) != nullptr)
        {
            if(!show_all && e->d_name[0] == '.')
                continue;
            names.emplace_back(e->d_name);
        }
        closedir(d);

        for(auto &name: names)
        {
            if(!long_format)
            {
                std::printf("%s  ", name.c_str());
                continue;
            }

            struct stat es;
            if(__vfs.stat(join(ap, name).c_str(), &es) == 0)
                std::printf("%s %8ld %s\n", mode_string(es.st_mode).c_str(),
                            (long)es.st_size, name.c_str());
            else
                std::printf("?         %s\n", name.c_str());
        }
        if(!long_format && !names.empty())
            std::printf("\n");
        if(multi)
            std::printf("\n");
    }
    return rc;
}

int cmd_cat(int argc, const char *argv[])
{
    std::vector<std::string_view> paths;

    argsparser parser;
    parser.add_argument("file").some_args().help("files to print")
        .action([&](std::string_view v){ paths.push_back(v); });

    int st = parse_args(parser, argc, argv);
    if(st != 0)
        return st < 0 ? 2 : 0;

    int rc = 0;
    for(auto v: paths)
    {
        std::string ap = absolute(v);
        int fd = __vfs.open(ap.c_str(), O_RDONLY);
        if(fd < 0)
        {
            report("cat", ap);
            rc = 1;
            continue;
        }

        char buf[256];
        for(;;)
        {
            auto n = __vfs.read(fd, buf, sizeof buf);
            if(n < 0)
            {
                report("cat", ap);
                rc = 1;
                break;
            }
            if(n == 0)
                break;
            std::fwrite(buf, 1, (size_t)n, stdout);
        }
        __vfs.close(fd);
    }
    std::fflush(stdout);
    return rc;
}

int cmd_touch(int argc, const char *argv[])
{
    std::vector<std::string_view> paths;

    argsparser parser;
    parser.add_argument("path").some_args().help("files to create")
        .action([&](std::string_view v){ paths.push_back(v); });

    int st = parse_args(parser, argc, argv);
    if(st != 0)
        return st < 0 ? 2 : 0;

    int rc = 0;
    for(auto v: paths)
    {
        std::string ap = absolute(v);
        struct stat sb;
        if(__vfs.stat(ap.c_str(), &sb) == 0)
            continue; // 没有时间戳可更新，存在即成功

        int fd = __vfs.open(ap.c_str(), O_CREAT | O_WRONLY);
        if(fd < 0)
        {
            report("touch", ap);
            rc = 1;
            continue;
        }
        __vfs.close(fd);
    }
    return rc;
}

int make_tree(const std::string &path)
{
    xeno::path p{path};
    if(p.is_root())
        return 0;

    std::vector<std::string> chain;
    while(!p.is_root())
    {
        chain.push_back(std::string{p});
        p = p.parent_path();
    }

    for(auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        if(__vfs.mkdir(it->c_str(), 0755) == 0)
            continue;
        if(errno != EEXIST)
        {
            report("mkdir", *it);
            return -1;
        }

        struct stat sb;
        if(__vfs.stat(it->c_str(), &sb) < 0 || !S_ISDIR(sb.st_mode))
        {
            report("mkdir", *it);
            return -1;
        }
    }
    return 0;
}

int cmd_mkdir(int argc, const char *argv[])
{
    bool parents = false;
    std::vector<std::string_view> paths;

    argsparser parser;
    parser.add_argument("-p", "--parents").flag().help("create parent directories")
        .action([&]{ parents = true; });
    parser.add_argument("path").some_args().help("directories to create")
        .action([&](std::string_view v){ paths.push_back(v); });

    int st = parse_args(parser, argc, argv);
    if(st != 0)
        return st < 0 ? 2 : 0;

    int rc = 0;
    for(auto v: paths)
    {
        std::string ap = absolute(v);
        if(parents)
        {
            if(make_tree(ap) < 0)
                rc = 1;
            continue;
        }
        if(__vfs.mkdir(ap.c_str(), 0755) < 0)
        {
            report("mkdir", ap);
            rc = 1;
        }
    }
    return rc;
}

int remove_path(const std::string &path, bool recursive, bool force)
{
    struct stat sb;
    if(__vfs.stat(path.c_str(), &sb) < 0)
    {
        if(force && errno == ENOENT)
            return 0;
        report("rm", path);
        return -1;
    }

    if(!S_ISDIR(sb.st_mode))
    {
        if(__vfs.unlink(path.c_str()) < 0)
        {
            report("rm", path);
            return -1;
        }
        return 0;
    }

    if(!recursive)
    {
        std::printf("rm: %s: is a directory\n", path.c_str());
        return -1;
    }

    DIR *d = opendir(path.c_str());
    if(d == nullptr)
    {
        report("rm", path);
        return -1;
    }

    // 先收集名字再关闭目录，避免边遍历边删除
    std::vector<std::string> names;
    struct dirent *e;
    while((e = readdir(d)) != nullptr)
        if(std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
            names.emplace_back(e->d_name);
    closedir(d);

    int rc = 0;
    for(auto &name: names)
        if(remove_path(join(path, name), true, force) < 0)
            rc = -1;

    if(__vfs.rmdir(path.c_str()) < 0)
    {
        report("rm", path);
        rc = -1;
    }
    return rc;
}

int cmd_rm(int argc, const char *argv[])
{
    bool recursive = false;
    bool force = false;
    std::vector<std::string_view> paths;

    argsparser parser;
    parser.add_argument("-r", "--recursive").flag().help("remove directories recursively")
        .action([&]{ recursive = true; });
    parser.add_argument("-f", "--force").flag().help("ignore missing files")
        .action([&]{ force = true; });
    parser.add_argument("path").some_args().help("files or directories to remove")
        .action([&](std::string_view v){ paths.push_back(v); });

    int st = parse_args(parser, argc, argv);
    if(st != 0)
        return st < 0 ? 2 : 0;

    int rc = 0;
    for(auto v: paths)
        if(remove_path(absolute(v), recursive, force) < 0)
            rc = 1;
    return rc;
}

int cmd_cd(int argc, const char *argv[])
{
    std::vector<std::string_view> paths;

    argsparser parser;
    parser.add_argument("path").any_args().help("directory to enter")
        .action([&](std::string_view v){ paths.push_back(v); });

    int st = parse_args(parser, argc, argv);
    if(st != 0)
        return st < 0 ? 2 : 0;
    if(paths.size() > 1)
    {
        std::printf("cd: too many arguments\n");
        return 2;
    }

    std::string target = paths.empty() ? "/" : absolute(paths[0]);
    struct stat sb;
    if(__vfs.stat(target.c_str(), &sb) < 0)
    {
        report("cd", target);
        return 1;
    }
    if(!S_ISDIR(sb.st_mode))
    {
        std::printf("cd: %s: not a directory\n", target.c_str());
        return 1;
    }

    cwd = std::move(target);
    return 0;
}

int cmd_pwd(int argc, const char *argv[])
{
    argsparser parser;
    int st = parse_args(parser, argc, argv);
    if(st != 0)
        return st < 0 ? 2 : 0;

    std::printf("%s\n", cwd.c_str());
    return 0;
}

} // namespace

namespace xeno
{

void syscmd_init()
{
    static bool initialized = false;
    if(initialized)
        return;
    initialized = true;

    struct command
    {
        const char *name;
        int (*function)(int, const char *[]);
    };

    static constexpr command commands[] = {
        {"ls",    cmd_ls},
        {"cat",   cmd_cat},
        {"touch", cmd_touch},
        {"mkdir", cmd_mkdir},
        {"rm",    cmd_rm},
        {"cd",    cmd_cd},
        {"pwd",   cmd_pwd},
    };

    for(auto &cmd: commands)
        shell_add(cmd.name, cmd.function);
}

}
