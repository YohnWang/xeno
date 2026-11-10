# xeno::argsparser 使用说明

单文件命令行参数解析器，header-only，位于 `xeno/toolkit/include/xeno/argsparser.h`。
所有名字位于 `namespace xeno`，头文件只依赖标准库，不抛异常、不使用 RTTI、没有全局状态，
也不依赖 `<format>` / `<print>`，可以在裸机上使用。

下文示例为简洁省略 `xeno::` 限定；实际使用时写 `xeno::argsparser`，或先
`using xeno::argsparser;`。

```cpp
#include<xeno/argsparser.h>
```

## 特性

* **Header only。** 单文件，除标准库外无依赖，不需要构建步骤。
* **无异常、无 RTTI、无全局状态。** 错误以 `status` 加一条消息的形式返回，所以能用
  `-fno-exceptions -fno-rtti` 编译运行。
* **不依赖 `<format>` 和 `<print>`。** 帮助文本用 `std::fwrite` 写、数字用
  `std::to_chars` 转；最小程序里库自身代码约 13 KB（见[设计说明](#设计说明)）。
* **全程 RAII。** 没有任何手工所有权；解析器本身就是持有 vector 和 string 的值类型。
* **值从不被拷贝。** 回调拿到的是直接指向 `argv` 的 `std::string_view`。
* **`-h/--help` 自动生成**，内容来自你注册的名字和 `.help()` 文本。
* **`parse_known_args()`** 把不认识的参数按原文顺序交回，正好用来转发给子命令。
* **可重复出现是显式选项**（`.append()`），因为重复给同一个选项通常意味着打错字。

## 快速开始

```cpp
#include<xeno/argsparser.h>
#include<cstdio>

int main(int argc, char *argv[])
{
    xeno::argsparser cmd;

    cmd.add_argument("-c", "--cache").flag().help("use the cache on the next run").action([] {
        std::printf("cache: on\n");
    });

    cmd.add_argument("-t", "--target").some_args().append().help("things to build").action([](std::string_view target) {
        std::printf("target: %.*s\n", static_cast<int>(target.size()), target.data());
    });

    cmd.add_argument("-j", "--jobs").one_args().help("parallel jobs").action([](std::string_view jobs) {
        std::printf("jobs: %.*s\n", static_cast<int>(jobs.size()), jobs.data());
    });

    cmd.add_argument("source").help("file to read").action([](std::string_view source) {
        std::printf("source: %.*s\n", static_cast<int>(source.size()), source.data());
    });

    switch (cmd.parse(argc, argv))
    {
    case xeno::argsparser::status::ok: break;
    case xeno::argsparser::status::help: return 0; // 给了 -h/--help，帮助已经打印过
    case xeno::argsparser::status::error: cmd.print_error(); return 2;
    }
}
```

```console
$ ./demo -c src.txt -t a b
cache: on
target: a
target: b
source: src.txt
```

`-h` 打印用法，内容由你注册的东西拼出来：

```console
$ ./demo -h
usage: demo [-h] [-c] [-t TARGET [TARGET ...]] [-j JOBS] SOURCE

positional arguments:
  SOURCE  file to read

options:
  -h, --help    show this help message and exit
  -c, --cache   use the cache on the next run
  -t, --target  things to build
  -j, --jobs    parallel jobs
```

用法错误写到 stderr，退出码由你自己决定：

```console
$ ./demo -t
usage: demo [-h] [-c] [-t TARGET [TARGET ...]] [-j JOBS] SOURCE
demo: error: argument '-t' expects a value
```

## 用列表传参数

除了 `(argc, argv)`，每个入口都接受 argv 形式的列表：花括号列表、
`std::vector<std::string>`、`std::vector<std::string_view>`、`std::array`，以及
`const char *` 的普通数组。**所有形式里第一个元素都是程序名**，和 shell 交给你的东西一模一样。

```cpp
cmd.parse({"demo", "-t", "main"});                        // 花括号列表
cmd.parse(std::vector<std::string>{"demo", "-t", "main"}); // std::vector<std::string>
cmd.parse(std::vector<std::string_view>{"demo", "-t"});    // std::vector<std::string_view>
cmd.parse(std::array<std::string, 2>{"demo", "-x"});       // std::array

std::vector<std::string> argv_like = read_somewhere();
cmd.parse(argv_like);                                      // 任何连续容器

const std::vector<std::string> rest = cmd.parse_known_args({"demo", "--nope"});
```

这个名字就是帮助里显示的程序名；空列表则保持原来的名字。如果第一个元素看起来像选项，会报错而不是
悄悄当成一个奇怪的程序名：

```cpp
cmd.parse({"-t", "main"});
// error: the first element of the list is the program name, but '-t' looks like an option
```

容器必须是连续的：`std::vector`、`std::array` 和 C 数组可以，`std::deque` 和 `std::list` 不行，
因为解析器按下标读这个列表。

## 值的个数

不特别声明的话，一个参数正好吃一个值：

| 注册写法 | 含义 | 等价于 |
| --- | --- | --- |
| `.flag()` | 开关，不吃值 | `.nargs(0)` |
| `.one_args()` | 正好一个值 | `.nargs(1)` |
| `.any_args()` | 零个或多个值 | `.nargs(0, xeno::argsparser::max)` |
| `.some_args()` | 一个或多个值 | `.nargs(1, xeno::argsparser::max)` |
| `.nargs(n)` | 正好 *n* 个值 | |
| `.nargs(lo, hi)` | *lo* 到 *hi* 个值 | |

`xeno::argsparser::max` 是上界的"无限"哨兵值。四个具名写法的意义只是可读性，每一个就是它旁边那个
`nargs()` 调用。

取值是**贪心**的：一个选项会一直吃后面的词，直到吃满上界，或者遇到看起来像选项的词
（`--something`、`-x`），或者遇到 `--`。所以贪心选项紧挨着位置参数时会把它一起吃掉：

```console
$ ./demo -c -t a b src.txt     # -t 把三个词全吃了
usage: demo [-h] [-c] [-t TARGET [TARGET ...]] [-j JOBS] SOURCE
demo: error: the following arguments are required: SOURCE
```

两种解法：把位置参数放到选项前面（`./demo -c src.txt -t a b`），或者用 `--` 隔开
（`./demo -c -t a b -- src.txt`）。两种都能用，而且位置参数并不必写在最前面——命令行上的顺序
是自由的。

以 `-` 开头但**长得像数字**的词永远不会被当成值之外的东西，所以负数值不用加引号：

```console
$ ./demo src.txt -j -4
jobs: -4
source: src.txt
```

## 重复的选项

选项默认只能出现**一次**。`.append()` 解除这个限制，各次出现按书写顺序处理，每个值调用一次回调：

```cpp
cmd.add_argument("-t", "--target").some_args().append()...
```

```console
$ ./demo src.txt -t a -t b
target: a
target: b
source: src.txt
```

不加 `.append()` 时，重复就是错误：

```console
$ ./demo src.txt -j 4 -j 8
usage: demo [-h] [-c] [-t TARGET [TARGET ...]] [-j JOBS] SOURCE
demo: error: argument '-j' cannot be given more than once (add .append() to allow it)
```

因为回调是"每个值调用一次"，对一个加了 `.append()` 的选项来说，`-t a b` 和 `-t a -t b`
的行为完全一样。

## 位置参数

任何不以 `-` 开头的名字都注册一个位置参数，且只能取一个名字：

```cpp
cmd.add_argument("source").help("file to read").action(...);
cmd.add_argument("extra").any_args().help("anything else").action(...);
```

位置参数按你注册的顺序，从"没有被任何选项认领的词"里依次填充。每一个都会尽量多拿，同时给后面的
兄弟留下足够的最小份额，所以"一个固定个数的位置参数 + 一个贪心的位置参数"能干净地切分：
`first` 拿一个词，`rest` 拿剩下的全部。

位置参数的回调在**所有选项处理完之后**才跑，所以输出顺序不等于它们在命令行上的出现顺序：

```console
$ ./demo -c src.txt -t a b
cache: on       <- -c，扫描过程中
target: a       <- -t，扫描过程中
target: b
source: src.txt <- 位置参数，最后
```

## 必需参数

选项默认是可选的。`.required()` 让它在没出现时解析失败：

```cpp
cmd.add_argument("-j", "--jobs").one_args().required()...
```

```console
$ ./demo src.txt
usage: demo [-h] [-c] [-t TARGET [TARGET ...]] -j JOBS SOURCE
demo: error: the following arguments are required: -j
source: src.txt
```

必需选项在用法行里也会去掉方括号。上面的检查发生在解析末尾，所以那时位置参数的回调已经跑过了——
前两行是 stderr，最后一行是 stdout。

位置参数不需要 `.required()`：它们的 `nargs` 下界已经说明了是否必须出现，缺失时会合并进同一条
提示消息。

## 动作回调

`.action(f)` 每个值调用一次，`f` 可以用任何一种顺手的形参形式接收值：

```cpp
.action([](std::string_view v) { ... });   // 零拷贝：指向 argv 的视图
.action([](const std::string &v) { ... }); // 一个临时 std::string
.action([](std::string v) { ... });        // 同样，按值接收
.action([](std::string &&v) { ... });      // 或右值引用：直接移动进来，不拷贝
.action([] { ... });                       // 完全不要值
```

按值那几种是直接把临时对象交过去（保证的复制消除），所以路上不发生拷贝；短值留在小字符串缓冲里，
不会分配。**非 const 左值引用**（`[](std::string &v)`）会在编译期被拒绝——值是个临时对象，没有对象
可供它引用。

返回值（如果有）会被忽略。对 `.flag()`，回调会带着一个空值调用一次——含义是"这个开关出现了"；
而一个没有被给定的可选位置参数则完全不调用。只注册、没写 `.action()` 的参数是惰性的，而不是
未定义行为。

捕获是支持的，而且**解析器持有可调用体的一份自己的副本**，所以按值捕获的状态属于这个参数，
而不属于你捕获的那个变量：

```cpp
int outside = 0;
cmd.add_argument("-n").one_args().action([count = 0, &outside](std::string_view) mutable {
    outside = ++count;   // count 是解析器那份副本，outside 是你的
});
```

这份副本活到解析器析构，所以能跨多次解析保留状态。要在外面看到状态就按引用（或指针）捕获；
状态本来就属于这个参数时就按值捕获。大于两个指针的捕获会放在回调内部的堆上，解析器被移动时
它跟着一起走。

move-only 可调用体也能用——比如捕获了 `std::unique_ptr` 的 lambda——但这时必须以右值传入：
`.action(std::move(f))`。传左值会要求拷贝，编译器会把原因说出来（`the action cannot be copied,
so it has to be given as an rvalue: pass std::move(f)`）。

## 语法形式

| 形式 | 含义 |
| --- | --- |
| `-c` | 短选项 |
| `-t value` | 带值的短选项 |
| `-tvalue`、`-t=value` | 同上，附着写法 |
| `--target value` | 带值的长选项 |
| `--target=value` | 同上，附着写法 |
| `-abc` | 合并的短开关——仅当 `-a -b -c` 全部注册且都不吃值时成立 |
| `--` | 它之后的一切都是位置参数的值 |
| `-1`、`-1.5`、`-.5`、`-2e-3`、`-0x1f` | 数字，永远不会被误认成选项 |

名字是字面匹配的：没有前缀缩写（`--tar` 不会匹配 `--target`），而且任何 token 只要不能**完整**
解析成某个选项，就整体不被拆开。正是最后这条让 `-cj4` 报错，而不是让 `-c` 半生效：

```console
$ ./demo src.txt -cj4
usage: demo [-h] [-c] [-t TARGET [TARGET ...]] [-j JOBS] SOURCE
demo: error: unrecognized arguments: -cj4
```

## 帮助与错误

`-h` 和 `--help` 由库自动注册。给出其中任一个都会打印帮助并返回
`xeno::argsparser::status::help`，解析就此停止。想自己用这两个名字，先
`xeno::argsparser cmd(false)` 构造——否则在自动注册的那个之上再注册 `-h` 会报"注册了两次"。

```cpp
cmd.print_help();    // 用法 + 位置参数 + 选项 -> stdout
cmd.print_usage();   // 只有用法行 -> stdout
cmd.print_error();   // 用法 + "prog: error: <消息>" -> stderr
cmd.error();         // 上一次失败解析的消息，成功时为空串
cmd.parser_status(); // 上一次 parse / parse_known_args 的返回状态
```

帮助里的程序名来自 `argv[0]`（也可以用 `cmd.program_name("x")` 设置）。

## 容忍不认识的参数

`parse_known_args()` 把认识的都解析掉，剩下的**返回**给你而不是报错——转发给子命令的程序正需要
这个：

```cpp
const std::vector<std::string> rest = cmd.parse_known_args(argc, argv);

if (cmd.parser_status() == xeno::argsparser::status::help) return 0;
if (cmd.parser_status() == xeno::argsparser::status::error) { cmd.print_error(); return 2; }
if (rest.empty()) return usage_error();

std::printf("dispatch to: %s\n", rest.front().c_str());
```

```console
$ ./forward -v build --jobs 4 -o out
verbose: on
dispatch to: build
  argument: --jobs
  argument: 4
  argument: -o
  argument: out
```

进入返回值的只有两类东西：不认识的选项，以及没有位置参数槽位接收的位置参数值。它们**按书写顺序**
返回，而且不认识的选项不会吞掉紧跟其后的词（上面的 `-v --jobs 4` 里，`--jobs` 被交回，`4` 跟在
它后面）。

除此之外的错误照旧：已知选项缺值、没加 `.append()` 却重复出现、缺必需参数、以及 `-h`。这些情况
下什么都不收集，`parser_status()` 是 `error`。返回的字符串是副本，所以就算来源 `argv` 已经销毁，
结果依然有效。

## API 速查

```cpp
namespace xeno
{
class argsparser
{
public:
    enum class status { ok, help, error };
    static constexpr std::size_t max;                    // nargs 的"无限"哨兵

    explicit argsparser(bool add_help = true);

    argument_ref add_argument(names...);                 // "-t", "--target" / "source"

    // 四种重载都是 argv 形式：第一个元素是程序名
    status parse(std::span<const std::string_view>);
    status parse(std::initializer_list<std::string_view>);      // parse({"demo", "-t"})
    template <class Range> status parse(const Range &);         // vector / array / ...
    status parse(int argc, const char *const argv[]);

    // parse_known_args() 有同样四个重载，返回不认识的参数
    std::vector<std::string> parse_known_args(...);

    status           parser_status() const noexcept;
    std::string_view error() const noexcept;
    void             print_help() const;                 // 另有 print_usage()、print_error()
    void             program_name(std::string_view);
    std::string_view program_name() const noexcept;
};
}
```

`add_argument()` 返回的是句柄而不是引用，所以你存下来的句柄不会因为之后又添加了参数而悬空。
每个注册方法都返回句柄，因此可以一路链式调用。

| 成员 | 作用 |
| --- | --- |
| `.flag()` `.one_args()` `.any_args()` `.some_args()` | 值的个数，见前面的表 |
| `.nargs(n)` / `.nargs(lo, hi)` | 精确 / 区间的值个数 |
| `.append()` | 允许该选项重复出现 |
| `.required()` | 该选项必须出现 |
| `.help(text)` | `-h` 里显示的那句话 |
| `.action(f)` | 每个值调用一次 |
| `.name()` | 主名字，例如 `"-t"` |
| `.occurrences()` | 上一次解析看到它的次数 |

| 解析器成员 | 作用 |
| --- | --- |
| `parse(span)` / `parse(argc, argv)` | 严格解析，返回 `status` |
| `parse_known_args(...)` | 返回不认识的参数，`std::vector<std::string>` |
| `parser_status()` | 上一次解析的 `status` |
| `error()` | 上一次失败解析的消息 |
| `print_help()` / `print_usage()` / `print_error()` | 输出 |
| `program_name()` | 读写帮助里用的程序名 |

## 设计说明

* **体积。** `-Os` 下，最小程序比"空 `main` 的同款程序"大约 34 KB；其中约 13 KB 是解析器自身的
  代码，剩下的是任何用 `std::string`/`std::vector` 的程序都要付的 libstdc++ 部分。若用
  `<format>`/`<print>` 来格式化帮助文本，要多付约 90 KB——这就是帮助文本手写拼接的原因。同理，
  `parse_known_args()` 对自己的两个已有序列表做归并，而不是调用 `std::sort`，后者会带进 1.7 KB
  的 introsort 机制。
* **`add_argument()` 在调用点就把类型擦掉了。** 它是个模板——`"-c"` 和 `"--cache"` 是不同类型，
  每一对名字都是独立实例化——但这个实例化只负责把名字收进一个 span，真正的活在**唯一一份**
  非模板重载里。所以一个调用点只花约 100 字节的转发壳，而不是整份注册逻辑的私有副本：`-Os`
  下实测每个调用点的 `.text` 从 1087 字节降到 108 字节，最小程序里库自身代码从 18 KB 降到 13 KB。
* **无异常。** 头文件里没有任何东西会抛，所以 `-fno-exceptions -fno-rtti` 可用（已验证，含
  `-static`）。
* **失败的解析除了 `error()` 之外不留痕**：某个出现次数最终被判定不完整时，它的回调一次都没跑；
  下一次解析从头开始干干净净。失败点**之前**已经处理完的出现次数是生效了的。
* **查找是线性扫描**：一个 CLI 也就几十个参数，省掉 `std::map`/`std::unordered_map` 能让代码和
  include 都保持小。
* **回调类型是 `std::move_only_function<void(std::string_view)>`**，它把小目标内联存放：
  在 libstdc++ 上实测，所有无捕获 lambda、以及两个指针以内的捕获，注册时**一次分配都不做**。
  回调必须持有自己的可调用体，因为 `parse()` 在 `.action(...)` 那条表达式结束很久之后才跑——
  这里换成 `std::function_ref` 会悬垂。

## 环境要求

头文件需要 C++23 编译器（用到 `std::move_only_function`、`std::span`、`std::to_chars` 和
concepts）；头文件本身既不需要 `<print>` 也不需要 `<format>`，只有自己的代码用
`std::println` 打印时才需要带 `<print>` 的标准库。

在 xeno 工程内，`xeno_toolkit` INTERFACE target 已导出 `xeno/toolkit/include`，链接它即可：

```cmake
target_link_libraries(your_target PRIVATE xeno_toolkit)
```

```cpp
#include<xeno/argsparser.h>
```

xeno 工程本身使用 C++26 / GCC 16。

## 这个解析器不做什么

* 没有子命令：用 `parse_known_args()` 加第二个解析器。
* 没有 `default_value` / "存进这个变量"：默认值写在回调捕获里，或者事后查 `.occurrences()`。
* 没有长选项前缀缩写，没有 `@file` 响应文件，没有环境变量。
* 不做 `int`/`double` 转换：回调拿到的是文本，转换是你自己的事。

## 验证

迁移后使用原仓库的完整自测 `test_argsparser.cc`（未随头文件迁入本仓库）编译运行：

- host（g++-16，C++26）：231 项检查全部通过。
- `arm-none-eabi-g++` 16.2.0 按工程同款 Cortex-M7 flags 做 `-fsyntax-only`：通过。
- `./build.sh` 构建与 `arm_simulator` 运行：正常，退出码 0。
