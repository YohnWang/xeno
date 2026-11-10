#pragma once

// ===========================================================================
//  argsparser - a small command line argument parser (header only).
//
//  All names in this header live in namespace xeno.
//
//  Design
//  ------
//  * One header, no dependencies beyond the standard library, **no** <format>
//    or <print>: the help text is written with std::fwrite and numbers with
//    std::to_chars.  That keeps the binary far smaller (the formatting
//    machinery is the single biggest thing a CLI parser usually drags in) and
//    makes the header usable on bare metal.
//  * No exceptions, no RTTI, no global state: errors are reported through the
//    returned status plus error().  Nothing in here can throw.
//  * RAII throughout, and nothing is owned by hand: the parser is a value
//    type holding vectors and strings, and the actions live in a
//    std::move_only_function that manages its own storage.
//  * Values are never copied: the parser hands std::string_view ranges of the
//    argv strings to the actions.  Action callbacks taking `const std::string&`
//    get a temporary std::string instead (small string optimisation applies).
//  * Lookup is a linear scan: a CLI has tens of arguments, and skipping
//    std::map/std::unordered_map keeps the code (and the includes) tiny.
//
//  Usage
//  -----
//      int main(int argc, char* argv[])
//      {
//          xeno::argsparser cmd;
//
//          // a flag: the action runs once, with an empty value
//          cmd.add_argument("-c", "--cache").flag().help("use the cache")
//             .action([](const std::string& s) { std::println("cache {}", s); });
//
//          // one or more values, repeatable: the action runs once per value,
//          // so `-t x1 x2` and `-t x1 -t x2` are the same
//          cmd.add_argument("-t", "--target").some_args().append()
//             .action([](std::string_view v) { std::println("target {}", v); });
//
//          // a positional argument; anything not starting with '-' is one
//          cmd.add_argument("source").help("file to read").action(...);
//
//          switch (cmd.parse(argc, argv))
//          {
//              case xeno::argsparser::status::ok:    break;
//              case xeno::argsparser::status::help:  return 0;   // -h/--help, already printed
//              case xeno::argsparser::status::error: cmd.print_error(); return 2;
//          }
//      }
//
//  parse() takes an argv-like list in any of these shapes, always with the
//  program name first, exactly as a shell hands it over:
//
//      cmd.parse(argc, argv);
//      cmd.parse({"demo", "-c", "src.txt"});
//      cmd.parse(std::vector<std::string>{"demo", "-c"});
//      cmd.parse(arr);                 // std::array or a plain const char* array
//
//  parse_known_args() has the same overloads.
//
//  Accepted syntax
//  ---------------
//      -c                  flag
//      -t value            short option with a value
//      -tvalue  -t=value   same, attached
//      --target value      long option with a value
//      --target=value      same, attached
//      -abc                bundled short flags (only if all of -a -b -c are
//                          registered and take no value)
//      --                  everything after it is a positional value
//      -h, --help          registered automatically unless argsparser(false);
//                          it prints the help and parse() returns status::help
//      -1  -1.5  -2e-3     numbers, so never mistaken for options
//
//  Guarantees
//  ----------
//  * An action only runs for a complete occurrence: a parse that returns
//    status::error has not run the action of the occurrence that failed, nor
//    anything after it.  (Occurrences parsed before the failure have run.)
//  * A failed parse leaves no other trace: the next parse() starts clean.
//  * parse() allocates nothing beyond the argument list itself.
//
//  How many values?
//  ----------------
//      .flag()         exactly 0, a switch      .nargs(0)
//      .one_args()     exactly 1                .nargs(1)
//      .any_args()     0 or more                .nargs(0, argsparser::max)
//      .some_args()    1 or more                .nargs(1, argsparser::max)
//      .nargs(n)       exactly n
//      .nargs(lo, hi)  between lo and hi, argsparser::max meaning "unbounded"
//
//  The four named ones only exist for readability; each is literally the
//  nargs() call beside it.  With none of them an argument takes exactly one
//  value.  For positionals the same vocabulary applies, except that .flag()
//  there would mean "an argument that never takes anything".
//
//  Tolerating what it does not know
//  --------------------------------
//      std::vector<std::string> unknown = cmd.parse_known_args(argc, argv);
//      if (cmd.parser_status() == argsparser::status::help) return 0;
//      if (!unknown.empty()) return run_subcommand(unknown);
//
//  parse_known_args() collects unrecognized options and surplus positional
//  values -- in the order they were written -- instead of failing on them;
//  everything else (a known option missing its value, a repeated one, a
//  missing required argument) is still an error, as is -h.  An unrecognized
//  option does not swallow the words after it: those are matched normally, so
//  they end up in the result only if nothing else claims them.  parser_status()
//  reports what the last parse() or parse_known_args() returned.
//
//  How many times?
//  ---------------
//  By default an option may be given once; a second occurrence is an error.
//  .append() lifts that.  Occurrences are then processed in order and each of
//  their values is handed to the action, so
//
//      cmd.add_argument("-t", "--target").some_args().append()...
//
//      -t x1 x2        ==  -t x1 -t x2        (two values, two calls)
//
//  Repeating is opt in, because a repeated option usually means a typo; the
//  ones that really are lists say so at their registration.
// ===========================================================================

#include<algorithm>
#include<charconv>
#include<concepts>
#include<cstddef>
#include<cstdio>
#include<functional>
#include<limits>
#include<span>
#include<string>
#include<string_view>
#include<type_traits>
#include<utility>
#include<vector>

namespace xeno
{

namespace argsparser_detail
{

    // --- number formatting ----------------------------------------------------
    //
    // std::to_chars, so nothing here needs <format> or a stream.

    inline void append_number(std::string &out, std::size_t value)
    {
        char buffer[20]; // 20 digits cover every std::size_t
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof buffer, value);
        out.append(buffer, static_cast<std::size_t>(result.ptr - buffer));
    }

    inline std::string number(std::size_t value)
    {
        std::string out;
        append_number(out, value);
        return out;
    }

    // "target" -> "TARGET", "using-cache" -> "USING_CACHE"
    inline std::string make_metavar(std::string_view name)
    {
        while (!name.empty() && name.front() == '-')
            name.remove_prefix(1);
        std::string out;
        out.reserve(name.size());
        for (char c : name)
        {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            else if (c == '-')
                c = '_';
            out.push_back(c);
        }
        return out;
    }

    // "[-t TARGET [TARGET ...]]" without the outer brackets, used for usage lines.
    // `name` is the option name, or the metavar itself for positional arguments.
    inline std::string render_usage(std::string_view name, std::string_view metavar,
                                    std::size_t lo, std::size_t hi, bool positional)
    {
        std::string out;
        if (hi > 0 || !positional)
            out = name; // positionals with nargs 0 render as nothing

        if (hi == 0)
            return out; // a flag: the name says it all

        if (lo == 0) // every value is optional
        {
            if (!out.empty())
                out += ' ';
            out += '[';
            out += metavar;
            out += (hi == 1) ? "]" : " ...]";
            return out;
        }

        // The first value placeholder of a positional is the name already emitted.
        const std::size_t emitted = positional ? 1 : 0;
        for (std::size_t k = emitted; k < lo; ++k)
        {
            out += ' ';
            out += metavar;
        }
        if (hi > lo)
        {
            out += " [";
            out += metavar;
            out += " ...]";
        }
        return out;
    }

    // --- the action callback ---------------------------------------------------
    //
    // std::move_only_function is exactly this type: erased, move-only, and it
    // stores small targets (every capture-less lambda) inline, so registering an
    // action usually does not allocate at all.  Empty is a valid state and is
    // checked before every call, so an argument registered without .action() is
    // simply inert.
    using callback = std::move_only_function<void(std::string_view)>;

    // Adapts whatever the user passed to .action() into `void(std::string_view)`.
    // std::string_view parameters are served straight from argv; a parameter of
    // type std::string / const std::string& / std::string&& gets a temporary
    // string, which is handed over without a further copy.  Values are always
    // passed as prvalues, so a callable taking `T&&` works too: the trait below
    // asks whether a prvalue is accepted, and the call has to match.
    template <class F>
    struct invoker
    {
        F fn;

        void operator()(std::string_view value)
        {
            if constexpr (std::invocable<F &, std::string_view>)
                fn(std::string_view{value});
            else if constexpr (std::invocable<F &, const std::string &>)
                fn(std::string{value});
            else if constexpr (std::invocable<F &, std::string>)
                fn(std::string{value});
            else
                fn();
        }
    };

    template <class F>
    constexpr bool invocable_action = std::invocable<F &, std::string_view> ||
                                      std::invocable<F &, const std::string &> ||
                                      std::invocable<F &, std::string> || std::invocable<F &>;

    inline bool is_number(std::string_view token) noexcept
    {
        // token starts with '-': "-1", "-1.5", "-.5", "-2e-3", "-0x1f" are numbers,
        // and therefore never mistaken for an option.
        std::string_view rest = token.substr(1);
        if (rest.empty())
            return false;
        if (rest.size() > 2 && rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X' || rest[1] == 'b' || rest[1] == 'B'))
            return true;

        std::size_t i = 0;
        bool digits = false, dot = false, exponent = false;
        for (; i < rest.size(); ++i)
        {
            const char c = rest[i];
            if (c >= '0' && c <= '9')
            {
                digits = true;
            }
            else if (c == '.' && !dot && !exponent)
            {
                dot = true;
            }
            else if ((c == 'e' || c == 'E') && digits && !exponent && i + 1 < rest.size())
            {
                exponent = true;
                digits = false;
                if (rest[i + 1] == '+' || rest[i + 1] == '-')
                    ++i;
            }
            else
            {
                return false;
            }
        }
        return digits;
    }

    // True for anything that must not be swallowed as a value: "-t", "--long", ...
    inline bool looks_like_option(std::string_view token) noexcept
    {
        return token.size() > 1 && token.front() == '-' && token != "--" && !is_number(token);
    }

// What parse() accepts as an argv-like list besides the (argc, argv) form: a
// contiguous container of string-like elements.  std::vector<std::string>,
// std::vector<std::string_view>, std::array<std::string, N> and a plain array
// of const char* qualify; a std::deque or std::list does not, because the
// parser reads the list by index.
template <class Range>
concept string_range = requires(const Range &range)
{
    std::size(range);
    requires std::convertible_to<std::remove_cvref_t<decltype(*std::data(range))>, std::string_view>;
};

} // namespace argsparser_detail

class argsparser
{
public:
    enum class status
    {
        ok,
        help,  // -h/--help was given (help was printed)
        error, // parse failed, see error()
    };

    // Sentinel for "unbounded": .nargs(1, argsparser::max)
    static constexpr std::size_t max = std::numeric_limits<std::size_t>::max();

    // -----------------------------------------------------------------------
    //  A non-owning handle to a registered argument.
    //
    //  add_argument() returns one of these instead of a reference into the
    //  parser's storage, so a handle that is stored away cannot dangle when
    //  more arguments are added later.  Handles stay valid as long as the
    //  parser does (nothing is ever removed), and are cheap to copy.
    // -----------------------------------------------------------------------
    class argument_ref
    {
        friend class argsparser;

        argsparser *owner_ = nullptr;
        std::size_t index_ = 0;

        argument_ref(argsparser &owner, std::size_t index) noexcept : owner_{&owner}, index_{index} {}

        bool live() const noexcept { return owner_ != nullptr && !owner_->arguments_[index_].invalid; }

        std::string label() const { return "'" + std::string(owner_->arguments_[index_].primary()) + "'"; }

    public:
        argument_ref() noexcept = default; // a null handle: every call is a no-op

        // Exactly `n` values.  .nargs(0) makes it a flag.
        argument_ref &nargs(std::size_t n) { return nargs(n, n); }

        // Between `lo` and `hi` values; use argsparser::max as "as many as
        // possible" (stops at the next token that looks like an option).
        argument_ref &nargs(std::size_t lo, std::size_t hi)
        {
            if (!live())
                return *this;
            if (lo > hi)
                owner_->fail_registration("argument " + label() + ": nargs(" + argsparser_detail::number(lo) +
                                          ", " + argsparser_detail::number(hi) + ") has min > max");
            else
            {
                auto &a = owner_->arguments_[index_];
                a.nargs_min = lo;
                a.nargs_max = hi;
            }
            return *this;
        }

        // The four shapes a CLI actually uses, named.  Each one is exactly the
        // nargs() call next to it, so there is only ever one code path.
        argument_ref &flag() { return nargs(0); }                       // a switch
        argument_ref &one_args() { return nargs(1); }                   // exactly one
        argument_ref &any_args() { return nargs(0, argsparser::max); }  // zero or more
        argument_ref &some_args() { return nargs(1, argsparser::max); } // one or more

        // Allow the option to be repeated.  Values accumulate: -t x1 -t x2 is
        // the same as -t x1 x2.  Without this a repeated option is an error.
        argument_ref &append()
        {
            if (live())
                owner_->arguments_[index_].appendable = true;
            return *this;
        }

        // The option must appear (meaningless for positionals, which use nargs).
        argument_ref &required()
        {
            if (live())
                owner_->arguments_[index_].needed = true;
            return *this;
        }

        argument_ref &help(std::string_view text)
        {
            if (live())
                owner_->arguments_[index_].help_text = text;
            return *this;
        }

        // The action runs once per value, with the value as std::string_view
        // (zero copy), as const std::string& / std::string (a temporary), or
        // with no parameter at all.  A flag runs it once with an empty value.
        // Captures are fine, and the parser keeps its own copy of the callable:
        // mutating a by-value capture inside the action changes that copy, not
        // the variable in the caller.  Capture by reference (or a pointer) to
        // see state outside; capturing by value means "state that belongs to
        // this argument".
        template <class F>
        argument_ref &action(F &&f)
        {
            using T = std::remove_cvref_t<F>;
            static_assert(argsparser_detail::invocable_action<T>,
                          "the action must be callable as f(std::string_view), f(std::string), "
                          "f(const std::string&) or f()");
            static_assert(std::is_constructible_v<T, F &&>,
                          "the action cannot be copied, so it has to be given as an rvalue: pass std::move(f)");
            if (live())
                owner_->arguments_[index_].action = argsparser_detail::callback{argsparser_detail::invoker<T>{std::forward<F>(f)}};
            return *this;
        }

        // The primary name, e.g. "-t" for add_argument("-t", "--target").
        std::string_view name() const { return live() ? owner_->arguments_[index_].primary() : std::string_view{}; }

        // How often it was seen by the last parse() (values for positionals).
        std::size_t occurrences() const { return live() ? owner_->arguments_[index_].occurrences : 0; }
    };

    explicit argsparser(bool add_help = true)
    {
        if (add_help)
        {
            const std::size_t index = arguments_.size();
            add_argument("-h", "--help").nargs(0).help("show this help message and exit");
            if (arguments_.size() > index)
                arguments_[index].is_help = true;
        }
    }

    argsparser(const argsparser &) = delete;
    argsparser &operator=(const argsparser &) = delete;
    argsparser(argsparser &&) = default;
    argsparser &operator=(argsparser &&) = default;

    // -----------------------------------------------------------------------
    //  Registration
    // -----------------------------------------------------------------------
    //
    //  add_argument("-t", "--target")  -> an option (any name starting with '-')
    //  add_argument("source")          -> a positional argument (exactly one name)
    //
    //  Names are matched literally; there is no prefix matching.
    //
    //  These two only gather the names and hand them to the non-template
    //  overload below.  That matters for size: a call site costs a handful of
    //  instructions instead of a private copy of the whole registration body
    //  (which was around 1.3 KB per call site at -Os, because "-c" and
    //  "--cache" are different types and each pair produced its own
    //  instantiation).
    template <class... Names>
        requires(sizeof...(Names) > 0) && (std::convertible_to<Names, std::string_view> && ...)
    argument_ref add_argument(Names &&...names)
    {
        const std::string_view list[] = {std::string_view(names)...};
        return add_argument(std::span<const std::string_view>{list, sizeof...(names)});
    }

    argument_ref add_argument(std::initializer_list<std::string_view> names)
    {
        return add_argument(std::span<const std::string_view>{names.begin(), names.size()});
    }

    // The one and only implementation of add_argument().
    argument_ref add_argument(std::span<const std::string_view> names)
    {
        if (names.empty()) return dead({}, "add_argument() needs at least one name");

        std::vector<std::string> list;
        list.reserve(names.size());
        for (const std::string_view name : names)
            list.emplace_back(name);

        const bool positional = !list.front().starts_with('-');
        for (const std::string &name : list)
        {
            if (name.size() < 2 && !positional)
                return dead(list.front(), "invalid option name '" + name + "'");
            if (positional && (name.empty() || name.starts_with('-')))
                return dead(name, "a positional argument cannot be named '" + name + "'");
            if (positional && list.size() > 1)
                return dead(name, "a positional argument takes exactly one name, got '" + name + "'");
            if (find(name))
                return dead(name, "argument '" + name + "' is registered twice");
        }

        argument entry;
        entry.names = std::move(list);
        entry.positional = positional;
        if (positional)
        {
            entry.nargs_min = 1;
            entry.nargs_max = 1;
        }
        // The longest name makes the nicest value placeholder: --target -> TARGET.
        std::string_view longest = entry.names.front();
        for (const std::string &n : entry.names)
            if (n.size() > longest.size())
                longest = n;
        entry.metavar = argsparser_detail::make_metavar(longest);

        const std::size_t index = arguments_.size();
        arguments_.push_back(std::move(entry));
        return argument_ref{*this, index};
    }

    // -----------------------------------------------------------------------
    //  Parsing
    // -----------------------------------------------------------------------

    // Every entry point takes an argv-like list: the first element is the
    // program name, exactly as a shell hands it over, and the rest are the
    // arguments.  parse(argc, argv) is the same thing spelled differently.

    status parse(std::span<const std::string_view> argv)
    {
        status_ = run(argv, false);
        return status_;
    }

    // cmd.parse({"demo", "-t", "main"})
    status parse(std::initializer_list<std::string_view> argv)
    {
        return parse(std::span<const std::string_view>{argv.begin(), argv.size()});
    }

    // ... and any other contiguous container of string-like elements:
    // std::vector<std::string>, std::vector<std::string_view>, std::array.
    template <class Range>
        requires argsparser_detail::string_range<Range>
    status parse(const Range &argv)
    {
        const std::vector<std::string_view> views = views_of(argv);
        return parse(std::span<const std::string_view>{views});
    }

    // The usual entry point: argv[0] becomes the program name in the help.
    status parse(int argc, const char *const argv[])
    {
        const std::vector<std::string_view> views = views_of(argc, argv);
        return parse(std::span<const std::string_view>{views});
    }

    // Like parse(), but anything the parser does not know is collected instead
    // of being an error: unrecognized options, and positional values that no
    // positional argument was left to take.  The two are returned in the order
    // they were given, which is what makes the result usable as the tail of a
    // command line for a subcommand to parse.  Every other failure -- a known
    // option missing its value, a repeated one, a missing required argument --
    // is still an error, reported the usual way.
    [[nodiscard]] std::vector<std::string> parse_known_args(std::span<const std::string_view> argv)
    {
        status_ = run(argv, true);
        return std::move(unknown_);
    }

    [[nodiscard]] std::vector<std::string> parse_known_args(std::initializer_list<std::string_view> argv)
    {
        return parse_known_args(std::span<const std::string_view>{argv.begin(), argv.size()});
    }

    template <class Range>
        requires argsparser_detail::string_range<Range>
    [[nodiscard]] std::vector<std::string> parse_known_args(const Range &argv)
    {
        const std::vector<std::string_view> views = views_of(argv);
        return parse_known_args(std::span<const std::string_view>{views});
    }

    [[nodiscard]] std::vector<std::string> parse_known_args(int argc, const char *const argv[])
    {
        const std::vector<std::string_view> views = views_of(argc, argv);
        return parse_known_args(std::span<const std::string_view>{views});
    }

    // The status of the last parse() / parse_known_args() call.
    status parser_status() const noexcept { return status_; }

    // -----------------------------------------------------------------------
    //  Results and help output
    // -----------------------------------------------------------------------

    // Empty unless the last parse() returned status::error.
    std::string_view error() const noexcept { return error_; }

    // The usage line and "prog: error: <message>", both on stderr.
    void print_error() const
    {
        const std::string text = usage_text() + prog_ + ": error: " + error_ + "\n";
        emit(stderr, text);
    }

    void print_usage() const { emit(stdout, usage_text()); }

    void print_help() const
    {
        emit(stdout, usage_text());

        bool any_positional = false;
        for (const argument &a : arguments_)
            if (a.positional && !a.invalid)
                any_positional = true;

        if (any_positional)
        {
            emit(stdout, "\npositional arguments:\n");
            print_rows(stdout, [](const argument &a)
                       { return a.positional; });
        }
        emit(stdout, "\noptions:\n");
        print_rows(stdout, [](const argument &a)
                   { return !a.positional; });
    }

    void program_name(std::string_view name)
    {
        const std::size_t slash = name.find_last_of("/\\");
        prog_ = std::string{slash == std::string_view::npos ? name : name.substr(slash + 1)};
        if (prog_.empty())
            prog_ = "program";
    }

    std::string_view program_name() const noexcept { return prog_; }

private:
    // One input token together with where it came from, so that whatever the
    // parser hands back can be put in the order the user wrote it.
    struct token
    {
        std::size_t at;
        std::string_view text;
    };

    struct argument
    {
        std::vector<std::string> names; // "-t", "--target", ...
        std::string help_text;
        std::string metavar; // "TARGET"
        std::size_t nargs_min = 1;
        std::size_t nargs_max = 1;
        std::size_t occurrences = 0;
        argsparser_detail::callback action;
        bool appendable = false;
        bool needed = false;
        bool positional = false;
        bool is_help = false;
        bool invalid = false; // registration failed

        std::string_view primary() const noexcept
        {
            return names.empty() ? std::string_view{} : std::string_view{names.front()};
        }
    };

    std::vector<argument> arguments_;
    std::vector<std::string_view> values_; // reused buffer: no allocation per option
    std::vector<token> pending_;           // unrecognized options, in source order
    std::vector<token> surplus_;           // positional values nobody took, in source order
    std::vector<std::string> unknown_;     // both of the above, merged, handed to the caller
    std::string error_;
    std::string prog_ = "program";
    status status_ = status::ok; // what the last parse returned
    bool fatal_ = false;         // a registration error happened; parse() can only fail

    static void emit(std::FILE *out, std::string_view text) noexcept
    {
        if (!text.empty())
            std::fwrite(text.data(), 1, text.size(), out);
    }

    // An argument without an action is inert rather than undefined.
    static void run(argument &a, std::string_view value)
    {
        if (a.action)
            a.action(value);
    }

    template <class... Parts>
    status fail(Parts &&...parts) // every part converts to std::string_view
    {
        (error_ += std::string_view(parts), ...);
        return status::error;
    }

    template <class... Parts>
    void fail_registration(Parts &&...parts)
    {
        fatal_ = true;
        (void)fail(std::forward<Parts>(parts)...);
    }

    // Record a registration failure and hand back an inert handle so that
    // chained calls keep compiling and do nothing.
    template <class... Parts>
    argument_ref dead(std::string_view name, Parts &&...parts)
    {
        fail_registration(std::forward<Parts>(parts)...);
        argument entry;
        entry.names.emplace_back(name);
        entry.positional = !name.starts_with('-');
        entry.metavar = argsparser_detail::make_metavar(name);
        entry.invalid = true;
        arguments_.push_back(std::move(entry));
        return argument_ref{*this, arguments_.size() - 1};
    }

    argument *find(std::string_view name) noexcept
    {
        for (argument &a : arguments_)
            for (const std::string &n : a.names)
                if (n == name)
                    return &a;
        return nullptr;
    }

    std::string usage_of(const argument &a) const
    {
        const std::string_view name = a.positional ? std::string_view{a.metavar} : a.primary();
        return argsparser_detail::render_usage(name, a.metavar, a.nargs_min, a.nargs_max, a.positional);
    }

    std::string row_head(const argument &a) const
    {
        if (a.positional)
            return a.metavar;
        std::string head;
        for (const std::string &n : a.names)
        {
            if (!head.empty())
                head += ", ";
            head += n;
        }
        return head;
    }

    std::string usage_text() const
    {
        std::string text = "usage: " + prog_;
        for (const argument &a : arguments_)
        {
            if (a.positional || a.invalid)
                continue;
            text += ' ';
            if (!a.needed)
                text += '[';
            text += usage_of(a);
            if (!a.needed)
                text += ']';
        }
        for (const argument &a : arguments_)
        {
            if (!a.positional || a.invalid)
                continue;
            std::string piece = usage_of(a);
            if (piece.empty())
                continue;
            text += ' ';
            text += piece;
        }
        return text + "\n";
    }

    template <class Predicate>
    void print_rows(std::FILE *out, Predicate show) const
    {
        std::size_t column = 4;
        for (const argument &a : arguments_)
        {
            if (!show(a) || a.invalid)
                continue;
            column = std::max(column, row_head(a).size() + 4);
        }
        column = std::min(column, std::size_t{32});

        std::string text;
        for (const argument &a : arguments_)
        {
            if (!show(a) || a.invalid)
                continue;
            const std::string head = row_head(a);
            text += "  ";
            text += head;
            if (head.size() + 2 >= column)
                text += "\n" + std::string(column, ' ');
            else
                text.append(column - head.size() - 2, ' ');
            text += a.help_text;
            text += '\n';
        }
        emit(out, text);
    }

    // argv -> a list of views, program name included.
    std::vector<std::string_view> views_of(int argc, const char *const argv[]) const
    {
        std::vector<std::string_view> views;
        if (argc > 0 && argv != nullptr)
        {
            views.reserve(static_cast<std::size_t>(argc));
            for (int i = 0; i < argc; ++i)
                views.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
        return views;
    }

    // Any other container of string-like elements -> a list of views.
    template <class Range>
        requires argsparser_detail::string_range<Range>
    static std::vector<std::string_view> views_of(const Range &argv)
    {
        std::vector<std::string_view> views;
        views.reserve(std::size(argv));
        for (const auto &element : argv)
            views.emplace_back(element);
        return views;
    }

    // The single implementation behind parse() and parse_known_args().
    // `argv` includes the program name, as a shell would hand it over.
    status run(std::span<const std::string_view> argv, bool keep_unknown)
    {
        if (fatal_)
            return status::error; // error_ still explains what went wrong
        error_.clear();
        unknown_.clear();
        pending_.clear();
        surplus_.clear();

        for (argument &a : arguments_)
            a.occurrences = 0;
        values_.clear();

        // Element 0 is the program name -- unless it is not there at all.
        if (!argv.empty())
        {
            if (argv.front().starts_with('-'))
                return fail("the first element of the list is the program name, but '", argv.front(),
                            "' looks like an option");
            program_name(argv.front());
            argv = argv.subspan(1);
        }

        std::vector<token> unclaimed; // values no option asked for
        bool literal = false;

        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            const std::string_view text = argv[i];
            if (literal || !(text.size() > 1 && text.front() == '-'))
            {
                unclaimed.push_back({i, text});
                continue;
            }
            if (text == "--")
            {
                literal = true;
                continue;
            }
            if (const status st = take_option(text, argv, i, unclaimed, keep_unknown); st != status::ok)
                return st;
        }

        if (const status st = assign_positionals(unclaimed, keep_unknown); st != status::ok)
            return st;
        if (keep_unknown)
            collect_unknown();
        return status::ok;
    }

    // Both runs are already in source order, so a merge is all that is needed.
    // std::sort would pull its introsort machinery (1.4 KB of .text) into the
    // binary for a list that is usually empty; an insertion sort would be
    // quadratic on a long forwarded command line.  This is neither.
    void collect_unknown()
    {
        std::size_t i = 0, j = 0;
        while (i < pending_.size() || j < surplus_.size())
        {
            const bool from_options = j == surplus_.size() ||
                                      (i < pending_.size() && pending_[i].at < surplus_[j].at);
            const token &t = from_options ? pending_[i++] : surplus_[j++];
            unknown_.emplace_back(t.text);
        }
    }

    // An unrecognized token: an error, or one more thing to hand back.
    status not_recognised(token unknown, bool keep_unknown)
    {
        if (keep_unknown)
        {
            pending_.push_back(unknown);
            return status::ok;
        }
        return fail("unrecognized arguments: ", unknown.text);
    }

    // One option token: resolve it, feed it its values, run the action.
    status take_option(std::string_view text, std::span<const std::string_view> args, std::size_t &i,
                       std::vector<token> &unclaimed, bool keep_unknown)
    {
        const std::size_t here = i; // consume() moves i along, remember where this token was

        if (argument *a = find(text); a != nullptr)
            return consume(*a, text, {}, false, args, i);

        if (argsparser_detail::is_number(text)) // a negative number is a value, not an option
        {
            unclaimed.push_back({here, text});
            return status::ok;
        }

        if (text.starts_with("--"))
        {
            if (const std::size_t eq = text.find('='); eq != std::string_view::npos)
            {
                const std::string_view name = text.substr(0, eq);
                if (argument *a = find(name); a != nullptr)
                    return consume(*a, name, text.substr(eq + 1), true, args, i);
                return not_recognised({here, text}, keep_unknown);
            }
            return not_recognised({here, text}, keep_unknown);
        }

        // Single dash: -tVALUE / -t=VALUE, then bundled flags -abc.
        if (text.size() > 2)
        {
            if (argument *a = find(text.substr(0, 2)); a != nullptr && a->nargs_max > 0)
            {
                std::string_view value = text.substr(2);
                if (value.starts_with('='))
                    value.remove_prefix(1);
                return consume(*a, text.substr(0, 2), value, true, args, i);
            }

            bool all_flags = true;
            for (std::size_t k = 1; k < text.size() && all_flags; ++k)
            {
                const char name[2] = {'-', text[k]};
                const argument *a = find(std::string_view{name, 2});
                all_flags = a != nullptr && a->nargs_max == 0;
            }
            if (all_flags)
            {
                for (std::size_t k = 1; k < text.size(); ++k)
                {
                    const char name[2] = {'-', text[k]};
                    const std::string_view one{name, 2};
                    if (const status st = consume(*find(one), one, {}, false, args, i); st != status::ok)
                        return st;
                }
                return status::ok;
            }
        }
        return not_recognised({here, text}, keep_unknown);
    }

    // `as` is the spelling the user actually typed, used in error messages.
    // Values are collected first and the action only runs once the occurrence
    // is known to be complete: a parse that fails changes nothing.
    status consume(argument &a, std::string_view as, std::string_view attached, bool has_attached,
                   std::span<const std::string_view> args, std::size_t &i)
    {
        if (a.is_help) // -h/--help: print it here, the caller just stops
        {
            print_help();
            return status::help;
        }

        if (++a.occurrences > 1 && !a.appendable)
            return fail("argument '", as, "' cannot be given more than once (add .append() to allow it)");

        if (has_attached && a.nargs_max == 0)
            return fail("argument '", as, "' does not take a value");

        // An attached value is just the first one: -t=x y and -t x y are alike.
        values_.clear();
        if (has_attached)
            values_.push_back(attached);
        while (values_.size() < a.nargs_max && i + 1 < args.size())
        {
            const std::string_view next = args[i + 1];
            if (next == "--" || argsparser_detail::looks_like_option(next))
                break;
            ++i;
            values_.push_back(next);
        }

        if (values_.empty() && a.nargs_min == 0)
            values_.push_back({}); // a flag: one call with an empty value

        if (values_.size() < a.nargs_min)
        {
            error_ += "argument '" + std::string{as} + "' expects ";
            if (a.nargs_min == 1)
                error_ += "a value";
            else
            {
                argsparser_detail::append_number(error_, a.nargs_min);
                error_ += " values";
                if (!values_.empty())
                {
                    error_ += ", got ";
                    argsparser_detail::append_number(error_, values_.size());
                }
            }
            return status::error;
        }

        for (const std::string_view value : values_)
            run(a, value);
        return status::ok;
    }

    status assign_positionals(std::span<const token> values, bool keep_unknown)
    {
        std::string missing;
        std::size_t next = 0;

        for (std::size_t k = 0; k < arguments_.size(); ++k)
        {
            argument &a = arguments_[k];
            if (!a.positional || a.invalid)
                continue;

            std::size_t reserved = 0; // keep enough values for the positionals after this one
            for (std::size_t j = k + 1; j < arguments_.size(); ++j)
                if (arguments_[j].positional && !arguments_[j].invalid)
                    reserved += arguments_[j].nargs_min;

            const std::size_t available = values.size() - next;
            std::size_t take = available > reserved ? available - reserved : 0;
            take = std::min(take, a.nargs_max);

            if (take < a.nargs_min) // the parse will fail: run nothing
            {
                if (!missing.empty())
                    missing += ", ";
                missing += a.metavar;
            }
            else
            {
                a.occurrences += take;
                for (std::size_t v = 0; v < take; ++v)
                    run(a, values[next + v].text);
            }
            next += take;
        }

        for (const argument &a : arguments_)
        {
            if (a.positional || a.invalid || !a.needed || a.occurrences > 0)
                continue;
            if (!missing.empty())
                missing += ", ";
            missing += a.primary();
        }

        if (next < values.size()) // values no positional was left to take
        {
            if (keep_unknown)
            {
                for (std::size_t v = next; v < values.size(); ++v)
                    surplus_.push_back(values[v]);
                return status::ok;
            }
            error_ += "unrecognized arguments:";
            for (std::size_t v = next; v < values.size(); ++v)
            {
                error_ += ' ';
                error_ += values[v].text;
            }
            return status::error;
        }
        if (!missing.empty())
            return fail("the following arguments are required: ", missing);
        return status::ok;
    }
};

} // namespace xeno
