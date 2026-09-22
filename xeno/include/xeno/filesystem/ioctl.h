#pragma once
#include<utility>
#if __has_include(<meta>)
#include<meta>
#endif
#include<concepts>

namespace xeno
{

template<typename T>
struct ioctlor
{
#ifdef __cpp_impl_reflection
    using parent_type = [:std::meta::parent_of(^^T):];
#else
    using parent_type = int;
#endif
    parent_type &parent;
    ioctlor(parent_type &p):parent{p}{}
};

#define regard_as_ioctl(class_name) \
static constexpr auto _reflect_current = std::meta::current_class();\
static constexpr auto _reflect_parent = std::meta::parent_of(_reflect_current);\
using parent_type = [:_reflect_parent:];\
parent_type &parent;\
class_name(parent_type &p):parent{p}{}\
static_assert(1)



template<typename O>
int ioctl_instance(auto &&fd, auto &&...args)
{
    auto &dev = static_cast<typename O::parent_type&>(fd);
#ifdef __cpp_impl_reflection
    template for(constexpr auto e : std::define_static_array(std::meta::nonstatic_data_members_of(^^ typename O::parent_type, std::meta::access_context::unchecked())))
    {
        using type = [:std::meta::type_of(e):];
        if constexpr(std::is_same_v<type, O>)
        {
            return fd.[:e:](args...);
            break;
        }
    }

#endif
    
    return 0;
}

#define ioctl(fd, op, ...) ioctl_instance<op>(fd __VA_OPT__(,) __VA_ARGS__)

}