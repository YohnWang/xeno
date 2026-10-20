#pragma once

#include<atomic>
#include<cctype>
#include<cstdio>
#include<iterator>
#include<string>
#include<string_view>
#include<functional>
#include<flat_map>
#include<thread>
#include<utility>
#include<vector>

#include<xeno/kits.h>


namespace xeno
{

template <size_t HistoryMax = 32>
struct basic_terminal
{
    // 补全回调：给出命令词前缀，返回所有匹配的命令名。
    using completer_t = std::function<std::vector<std::string_view>(std::string_view)>;

    std::string order;
    std::string buf;
    int char_index_next = 0;
    completer_t completer;
    std::string_view prompt; // 列出候选后重绘命令行时使用，可为空

    static constexpr size_t history_max = HistoryMax;
    xeno::static_queue<std::string, HistoryMax> history; // 0 为最旧，末尾为最新
    int history_index = -1; // -1 表示正在编辑新行，0 表示最新一条历史
    std::string pending;    // 浏览历史前正在编辑的行，向下翻回时恢复
    int arrow_key_state = 0;

    void push(int ch)
    {
        buf.insert(std::next(buf.begin(), char_index_next), ch);
        for(size_t i = char_index_next; i < buf.size(); i++)
            putchar(buf[i]);
        for(size_t i = buf.size() - 1; i > (size_t)char_index_next; i--)
            putchar('\b');
        char_index_next++;
        fflush(stdout);
    }

    void pop()
    {
        if(char_index_next > 0)
        {
            putchar('\b');
            for(size_t i = char_index_next; i < buf.size(); i++)
                putchar(buf[i]);
            putchar(' ');
            for(int i = (int)buf.size(); i >= char_index_next; i--)
                putchar('\b');
            buf.erase(std::next(buf.begin(), char_index_next - 1));
            char_index_next--;
            fflush(stdout);
        }
    }

    void left()
    {
        if(char_index_next > 0)
        {
            putchar('\b');
            char_index_next--;
            fflush(stdout);
        }
    }

    void right()
    {
        if(char_index_next < (int)buf.size())
        {
            putchar(buf[char_index_next]);
            char_index_next++;
            fflush(stdout);
        }
    }

    void history_push(const std::string &s)
    {
        if(history.size() > 0 && history[history.size() - 1] == s)
            return;
        history.push_overwrite(s);
    }

    void clear_bufview()
    {
        if(buf.empty())
            return;
        for(int i = 0; i < char_index_next; i++)
            putchar('\b');
        for(size_t i = 0; i < buf.size(); i++)
            putchar(' ');
        for(size_t i = 0; i < buf.size(); i++)
            putchar('\b');
        char_index_next = 0;
        fflush(stdout);
    }

    void show_line(std::string_view s)
    {
        clear_bufview();
        buf.assign(s);
        char_index_next = buf.size();
        for(auto i: buf)
            putchar(i);
        fflush(stdout);
    }

    void history_up()
    {
        if(history.size() == 0)
            return;

        if(history_index < 0)
        {
            pending = buf;
            history_index = 0;
        }
        else if((size_t)(history_index + 1) < history.size())
        {
            history_index++;
        }
        else
        {
            return;
        }

        show_line(history[history.size() - 1 - history_index]);
    }

    void history_down()
    {
        if(history_index < 0)
            return;

        history_index--;
        if(history_index < 0)
        {
            // 翻过最新一条后恢复浏览历史之前正在编辑的内容。
            show_line(pending);
            pending.clear();
        }
        else
        {
            show_line(history[history.size() - 1 - history_index]);
        }
    }

    void show_prompt()
    {
        for(char c: prompt)
            putchar(c);
        fflush(stdout);
    }

    // 在下一行列出所有候选，然后重绘提示符和当前输入。
    void list_candidates(const std::vector<std::string_view> &candidates)
    {
        putchar('\n');
        for(size_t i = 0; i < candidates.size(); i++)
        {
            if(i > 0)
            {
                putchar(' ');
                putchar(' ');
            }
            for(char c: candidates[i])
                putchar(c);
        }
        putchar('\n');

        show_prompt();
        for(char c: buf)
            putchar(c);
        for(size_t i = buf.size(); i > (size_t)char_index_next; i--)
            putchar('\b');
        fflush(stdout);
    }

    // 补全光标所在的命令词。多个候选时补到公共前缀并列出全部候选。
    void complete()
    {
        if(!completer || char_index_next <= 0)
            return;

        // 跳过行首空白，只补全第一个命令词
        int begin = 0;
        while(begin < (int)buf.size() && isblank((unsigned char)buf[begin]))
            begin++;
        if(char_index_next <= begin)
            return;

        for(int i = begin; i < char_index_next; i++)
            if(isblank((unsigned char)buf[i]))
                return; // 光标已在参数区

        std::string_view prefix(buf.data() + begin, char_index_next - begin);
        std::vector<std::string_view> candidates = completer(prefix);
        if(candidates.empty())
        {
            putchar('\a');
            fflush(stdout);
            return;
        }

        std::string_view common = candidates.front();
        for(auto candidate: candidates)
        {
            size_t n = 0;
            while(n < common.size() && n < candidate.size()
                  && common[n] == candidate[n])
                n++;
            common = common.substr(0, n);
        }

        for(size_t i = prefix.size(); i < common.size(); i++)
            push(common[i]);

        if(candidates.size() == 1)
            push(' ');
        else
            list_candidates(candidates);
    }

    enum class status
    {
        none = 0,
        ready = 1,
        newline = 2
    };

    status read_one_char(int ch)
    {
        if(ch == 0x1b)
        {
            arrow_key_state = -1;
        }
        else if(arrow_key_state == -1)
        {
            if(ch == '[')
                arrow_key_state = -2;
            else
                arrow_key_state = 0;
        }
        else if(arrow_key_state == -2)
        {
            if(ch == 'A')
                history_up();
            else if(ch == 'B')
                history_down();
            else if(ch == 'C')
                right();
            else if(ch == 'D')
                left();
            arrow_key_state = 0;
        }
        else if(ch == '\t')
        {
            complete();
        }
        else if(isprint(ch))
        {
            push(ch);
        }
        else if(ch == '\n' || ch == '\r')
        {
            putchar('\n');
            history_index = -1;
            pending.clear();
            if(!buf.empty())
            {
                history_push(buf);
                order = buf;
                buf.clear();
                char_index_next = 0;
                return status::ready;
            }
            return status::newline;
        }
        else if(ch == 0x7F || ch == '\b')
        {
            pop();
        }

        return status::none;
    }

    std::string_view getbuf() const
    {
        return order;
    }
};

using terminal = basic_terminal<>;

inline std::vector<std::string> string_split(std::string_view s)
{
    std::vector<std::string> v;
    size_t begin = 0, end = 0;
    for(end = 0; end < s.size(); end++)
    {
        if(isblank((unsigned char)s[end]))
        {
            if(begin < end)
                v.emplace_back(s.substr(begin, end - begin));
            begin = end + 1;
        }
    }
    if(begin < end)
        v.emplace_back(s.substr(begin, end - begin));
    return v;
}

// 命令识别与分发，不涉及终端 I/O。
struct shell
{
    using command_function_t = std::function<int (int argc, const char *argv[])>;

    struct command_entry
    {
        command_function_t function;
        bool authorized = false; // 整体授权关闭时是否豁免
    };

    std::flat_map<std::string, command_entry> commands;
    bool shell_authorized = true;

    shell()
    {
        add("help", [this](int, const char *[]){
            std::puts("shell commands:");
            for(auto &&[name, cmd]: commands)
                if(visible(cmd))
                    std::puts(name.c_str());
            return 0;
        });
        authorize("help", true);
    }

    bool visible(const command_entry &cmd) const
    {
        return shell_authorized || cmd.authorized;
    }

    // 把本 shell 的命令补全接到终端上。
    template <size_t N>
    void bind(basic_terminal<N> &term)
    {
        term.completer = [this](std::string_view prefix){
            return complete(prefix);
        };
    }

    std::vector<std::string_view> complete(std::string_view prefix) const
    {
        std::vector<std::string_view> matches;
        for(auto &&[name, cmd]: commands)
            if(visible(cmd) && name.starts_with(prefix))
                matches.push_back(name);
        return matches;
    }

    int add(std::string name, command_function_t f)
    {
        if(commands.contains(name))
        {
            std::printf("command %s already exists.\n", name.c_str());
            return -1;
        }
        commands.insert({std::move(name), command_entry{std::move(f)}});
        return 0;
    }

    int execute(std::string_view s)
    {
        auto v = string_split(s);
        if(v.empty())
            return 0;

        auto it = commands.find(v[0]);
        if(it == commands.end() || !visible(it->second))
        {
            std::printf("%s: command not found\n", v[0].c_str());
            return 127;
        }

        std::vector<const char *> args;
        args.reserve(v.size());
        for(auto &i: v)
            args.push_back(i.c_str());
        return it->second.function((int)args.size(), args.data());
    }

    // 控制整体shell命令是否可见可执行，shell构造时默认为可以
    void authorize(bool enable = true)
    {
        shell_authorized = enable;
    }

    // 赋予命令在整体shell授权关闭时，指定的命令可获得豁免，默认命令无授权，需显示调用该接口
    void authorize(std::string_view cmd, bool enable = true)
    {
        auto it = commands.find(std::string(cmd));
        if(it == commands.end())
        {
            std::printf("command %.*s not found.\n", (int)cmd.size(), cmd.data());
            return;
        }
        it->second.authorized = enable;
    }

    void authorize(const char *cmd, bool enable = true)
    {
        authorize(std::string_view(cmd), enable);
    }

};

inline auto& __shell()
{
    static shell sh;
    return sh;
}

// 默认 shell 的运行状态。exit 命令会一直留在命令表里，状态必须放在静态区。
inline std::atomic<bool>& __shell_running()
{
    static std::atomic<bool> running{false};
    return running;
}

// 查询默认 shell 的循环是否在运行，可在任意线程调用。
inline bool shell_running()
{
    return __shell_running();
}

inline void shell_add(std::string_view name, shell::command_function_t func)
{
    __shell().add(std::string(name), std::move(func));
}

inline void shell_add(const char *name, shell::command_function_t func)
{
    shell_add(std::string_view(name), std::move(func));
}

inline void shell_authorize(std::string_view cmd, bool enable = true)
{
    __shell().authorize(cmd, enable);
}

inline void shell_authorize(const char *cmd, bool enable = true)
{
    shell_authorize(std::string_view(cmd), enable);
}

inline void shell_start()
{
    // exit 命令会一直留在命令表里，状态放在静态区，避免引用悬空。
    __shell_running() = true;

    std::jthread th{[](){
        shell &sh = __shell();
        terminal term;
        term.prompt = "# ";
        sh.bind(term);

        // 应用已经注册了 exit 就沿用应用的行为，不重复添加。
        if(!sh.commands.contains("exit"))
            sh.add("exit", [](int, const char *[]){
                __shell_running() = false;
                return 0;
            });

        term.show_prompt();
        while(__shell_running())
        {
            int ch = std::getchar();
            if(ch == EOF)
                break;

            auto st = term.read_one_char(ch);
            if(st == terminal::status::ready)
                sh.execute(term.getbuf());
            if(st != terminal::status::none && __shell_running())
                term.show_prompt();
        }

        // EOF 退出时也要同步状态。
        __shell_running() = false;
    }};
    th.detach();
}

}
