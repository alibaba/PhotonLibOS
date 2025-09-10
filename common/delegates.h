#include <photon/common/PMF.h>
#include <photon/common/callback.h>

#include <type_traits>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"

namespace photon {

struct DUtils;

// Do the foul deed here
class DelegatesBase {
protected:
    void *__objptr;

public:
    template <typename T>
    DelegatesBase(T &obj) : __objptr(&obj) {}
    DelegatesBase(void *obj) : __objptr(obj) {}
    DelegatesBase() : __objptr(nullptr) {}
    friend DUtils;
};

struct SelfRef {};

struct AccessorBase {
    AccessorBase() = default;
    template <typename T>
    explicit AccessorBase(T &&){};
};

// Using typename to stores function name

#define DEFINE_DELEGATE_FUNCTION(name, funcname)                               \
    struct name {                                                              \
        template <typename Parent, typename FacadeType, typename Rt,           \
                  typename... Args>                                            \
        class accessor : public Parent {                                       \
        protected:                                                             \
            using __signature = photon::FuncSignature<Rt(Args...)>;            \
            Rt (*__funcptr)(void *, Args...);                                  \
                                                                               \
        public:                                                                \
            accessor(const accessor &rhs)                                      \
                : Parent(rhs), __funcptr(rhs.__funcptr) {}                     \
            template <typename T, typename = std::enable_if_t<                 \
                                      !std::is_base_of<accessor, T>::value>>   \
            accessor(T &obj)                                                   \
                : Parent(obj),                                                 \
                  __funcptr(reinterpret_cast<Rt (*)(void *, Args...)>(         \
                      ::get_member_function_address<T, Rt>(&obj, &T::funcname) \
                          .f)) {}                                              \
            accessor() : Parent(), __funcptr(nullptr) {}                       \
            Rt funcname(Args... args) {                                        \
                return __funcptr                                               \
                           ? __funcptr(photon::DUtils::__get_obj(              \
                                           static_cast<FacadeType &>(*this)),  \
                                       args...)                                \
                           : Rt{};                                             \
            }                                                                  \
            using Parent::funcname;                                            \
            using __ParentAccessor = Parent;                                   \
            friend photon::DUtils;                                             \
            template <typename T>                                              \
            friend class photon::FuncSignature;                                \
        };                                                                     \
        template <typename Parent, typename FacadeType, typename... Args>      \
        class accessor<Parent, FacadeType, void, Args...> : public Parent {    \
        protected:                                                             \
            using __signature = photon::FuncSignature<void(Args...)>;          \
            void (*__funcptr)(void *, Args...);                                \
                                                                               \
        public:                                                                \
            accessor(const accessor &rhs)                                      \
                : Parent(rhs), __funcptr(rhs.__funcptr) {}                     \
            template <typename T, typename = std::enable_if_t<                 \
                                      !std::is_base_of<accessor, T>::value>>   \
            accessor(T &obj)                                                   \
                : Parent(obj),                                                 \
                  __funcptr(reinterpret_cast<void (*)(void *, Args...)>(       \
                      ::get_member_function_address(                           \
                          &obj, (void(T::*)(Args...)) & T::funcname)           \
                          .f)) {}                                              \
            accessor() : Parent(), __funcptr(nullptr) {}                       \
            void funcname(Args... args) {                                      \
                if (__funcptr)                                                 \
                    __funcptr(photon::DUtils::__get_obj(                       \
                                  static_cast<FacadeType &>(*this)),           \
                              args...);                                        \
            }                                                                  \
            using Parent::funcname;                                            \
            using __ParentAccessor = Parent;                                   \
            friend photon::DUtils;                                             \
            template <typename T>                                              \
            friend class photon::FuncSignature;                                \
        };                                                                     \
        template <typename Parent, typename FacadeType, typename... Args>      \
        class accessor<Parent, FacadeType, photon::SelfRef, Args...>           \
            : public Parent {                                                  \
        protected:                                                             \
            using __signature =                                                \
                photon::FuncSignature<photon::SelfRef(Args...)>;               \
            void *(*__funcptr)(void *, Args...);                               \
                                                                               \
        public:                                                                \
            accessor(const accessor &rhs)                                      \
                : Parent(rhs), __funcptr(rhs.__funcptr) {}                     \
            template <typename T, typename = std::enable_if_t<                 \
                                      !std::is_base_of<accessor, T>::value>>   \
            accessor(T &obj)                                                   \
                : Parent(obj),                                                 \
                  __funcptr(reinterpret_cast<void *(*)(void *, Args...)>(      \
                      ::get_member_function_address(&obj, &T::funcname).f)) {} \
            accessor() : Parent(), __funcptr(nullptr) {}                       \
            FacadeType funcname(Args... args) {                                \
                void *obj =                                                    \
                    __funcptr                                                  \
                        ? __funcptr(photon::DUtils::__get_obj(                 \
                                        static_cast<FacadeType &>(*this)),     \
                                    args...)                                   \
                        : nullptr;                                             \
                FacadeType ret = *(FacadeType *)this;                          \
                photon::DUtils::__set_obj(ret, obj);                           \
                return ret;                                                    \
            }                                                                  \
            using Parent::funcname;                                            \
            using __ParentAccessor = Parent;                                   \
            friend photon::DUtils;                                             \
            template <typename T>                                              \
            friend class photon::FuncSignature;                                \
        };                                                                     \
        template <typename FacadeType, typename Rt, typename... Args>          \
        class accessor<photon::AccessorBase, FacadeType, Rt, Args...>          \
            : public photon::AccessorBase {                                    \
        protected:                                                             \
            using __signature = photon::FuncSignature<Rt(Args...)>;            \
            Rt (*__funcptr)(void *, Args...);                                  \
                                                                               \
        public:                                                                \
            accessor(const accessor &rhs) : __funcptr(rhs.__funcptr) {}        \
            template <typename T, typename = std::enable_if_t<                 \
                                      !std::is_base_of<accessor, T>::value>>   \
            accessor(T &obj)                                                   \
                : __funcptr(reinterpret_cast<Rt (*)(void *, Args...)>(         \
                      ::get_member_function_address<T, Rt>(&obj, &T::funcname) \
                          .f)) {}                                              \
            accessor() : __funcptr(nullptr) {}                                 \
            Rt funcname(Args... args) {                                        \
                return __funcptr                                               \
                           ? __funcptr(photon::DUtils::__get_obj(              \
                                           static_cast<FacadeType &>(*this)),  \
                                       args...)                                \
                           : Rt{};                                             \
            }                                                                  \
            using __ParentAccessor = AccessorBase;                             \
            friend photon::DUtils;                                             \
            template <typename T>                                              \
            friend class photon::FuncSignature;                                \
        };                                                                     \
        template <typename FacadeType, typename... Args>                       \
        class accessor<photon::AccessorBase, FacadeType, void, Args...>        \
            : public photon::AccessorBase {                                    \
        protected:                                                             \
            using __signature = photon::FuncSignature<void(Args...)>;          \
            void (*__funcptr)(void *, Args...);                                \
                                                                               \
        public:                                                                \
            accessor(const accessor &rhs) : __funcptr(rhs.__funcptr) {}        \
            template <typename T, typename = std::enable_if_t<                 \
                                      !std::is_base_of<accessor, T>::value>>   \
            accessor(T &obj)                                                   \
                : __funcptr(reinterpret_cast<void (*)(void *, Args...)>(       \
                      ::get_member_function_address(                           \
                          &obj, (void(T::*)(Args...)) & T::funcname)           \
                          .f)) {}                                              \
            accessor() : __funcptr(nullptr) {}                                 \
            void funcname(Args... args) {                                      \
                if (__funcptr)                                                 \
                    __funcptr(photon::DUtils::__get_obj(                       \
                                  static_cast<FacadeType &>(*this)),           \
                              args...);                                        \
            }                                                                  \
            using __ParentAccessor = AccessorBase;                             \
            friend photon::DUtils;                                             \
            template <typename T>                                              \
            friend class photon::FuncSignature;                                \
        };                                                                     \
        template <typename FacadeType, typename... Args>                       \
        class accessor<photon::AccessorBase, FacadeType, photon::SelfRef,      \
                       Args...> : public photon::AccessorBase {                \
        protected:                                                             \
            using __signature =                                                \
                photon::FuncSignature<photon::SelfRef(Args...)>;               \
            void *(*__funcptr)(void *, Args...);                               \
                                                                               \
        public:                                                                \
            accessor(const accessor &rhs) : __funcptr(rhs.__funcptr) {}        \
            template <typename T, typename = std::enable_if_t<                 \
                                      !std::is_base_of<accessor, T>::value>>   \
            accessor(T &obj)                                                   \
                : __funcptr(reinterpret_cast<void *(*)(void *, Args...)>(      \
                      ::get_member_function_address(&obj, &T::funcname).f)) {} \
            accessor() : __funcptr(nullptr) {}                                 \
            FacadeType funcname(Args... args) {                                \
                void *obj =                                                    \
                    __funcptr                                                  \
                        ? __funcptr(photon::DUtils::__get_obj(                 \
                                        static_cast<FacadeType &>(*this)),     \
                                    args...)                                   \
                        : nullptr;                                             \
                FacadeType ret = *(FacadeType *)this;                          \
                photon::DUtils::__set_obj(ret, obj);                           \
                return ret;                                                    \
            }                                                                  \
            using __ParentAccessor = photon::AccessorBase;                     \
            friend photon::DUtils;                                             \
            template <typename T>                                              \
            friend class photon::FuncSignature;                                \
        };                                                                     \
    };

#define DEFINE_DELEGATE_UNARY_OP(name, funcname)                              \
    struct name {                                                             \
        template <typename Parent, typename FacadeType, typename Rt>          \
        class accessor : public Parent {                                      \
        protected:                                                            \
            using __signature = photon::FuncSignature<Rt()>;                  \
            Rt (*__funcptr)(void *);                                          \
                                                                              \
        public:                                                               \
            accessor(const accessor &rhs)                                     \
                : Parent(rhs), __funcptr(rhs.__funcptr) {}                    \
            template <typename T, typename = std::enable_if_t<                \
                                      !std::is_base_of<accessor, T>::value>>  \
            accessor(T &obj)                                                  \
                : __funcptr(reinterpret_cast<Rt (*)(void *)>(                 \
                      ::get_member_function_address<T, Rt>(                   \
                          &obj, &T::operator funcname)                        \
                          .f)) {}                                             \
            accessor() : Parent(), __funcptr(nullptr) {}                      \
            Rt operator funcname() {                                          \
                return __funcptr ? __funcptr(photon::DUtils::__get_obj(       \
                                       static_cast<FacadeType &>(*this)))     \
                                 : Rt{};                                      \
            }                                                                 \
            using __ParentAccessor = Parent;                                  \
            friend photon::DUtils;                                            \
            template <typename T>                                             \
            friend class photon::FuncSignature;                               \
        };                                                                    \
        template <typename Parent, typename FacadeType>                       \
        class accessor<Parent, FacadeType, void> : public Parent {            \
        protected:                                                            \
            using __signature = photon::FuncSignature<void()>;                \
            void (*__funcptr)(void *);                                        \
                                                                              \
        public:                                                               \
            accessor(const accessor &rhs)                                     \
                : Parent(rhs), __funcptr(rhs.__funcptr) {}                    \
            template <typename T, typename = std::enable_if_t<                \
                                      !std::is_base_of<accessor, T>::value>>  \
            accessor(T &obj)                                                  \
                : Parent(obj),                                                \
                  __funcptr(reinterpret_cast<void (*)(void *)>(               \
                      ::get_member_function_address(                          \
                          &obj, (void(T::*)()) & T::operator funcname)        \
                          .f)) {}                                             \
            accessor() : Parent(), __funcptr(nullptr) {}                      \
            void operator funcname() {                                        \
                if (__funcptr)                                                \
                    __funcptr(photon::DUtils::__get_obj(                      \
                        static_cast<FacadeType &>(*this)));                   \
            }                                                                 \
            using __ParentAccessor = Parent;                                  \
            friend photon::DUtils;                                            \
            template <typename T>                                             \
            friend class photon::FuncSignature;                               \
        };                                                                    \
        template <typename Parent, typename FacadeType>                       \
        class accessor<Parent, FacadeType, photon::SelfRef> : public Parent { \
        protected:                                                            \
            using __signature = photon::FuncSignature<photon::SelfRef()>;     \
            void *(*__funcptr)(void *);                                       \
                                                                              \
        public:                                                               \
            accessor(const accessor &rhs)                                     \
                : Parent(rhs), __funcptr(rhs.__funcptr) {}                    \
            template <typename T, typename = std::enable_if_t<                \
                                      !std::is_base_of<accessor, T>::value>>  \
            accessor(T &obj)                                                  \
                : Parent(obj),                                                \
                  __funcptr(reinterpret_cast<void *(*)(void *)>(              \
                      ::get_member_function_address(&obj,                     \
                                                    &T::operator funcname)    \
                          .f)) {}                                             \
            accessor() : Parent(), __funcptr(nullptr) {}                      \
            FacadeType operator funcname() {                                  \
                void *obj = __funcptr ? __funcptr(photon::DUtils::__get_obj(  \
                                            *(FacadeType *)this))             \
                                      : nullptr;                              \
                FacadeType ret = static_cast<FacadeType &>(*this);            \
                photon::DUtils::__set_obj(ret, obj);                          \
                return ret;                                                   \
            }                                                                 \
            using __ParentAccessor = Parent;                                  \
            friend photon::DUtils;                                            \
            template <typename T>                                             \
            friend class photon::FuncSignature;                               \
        };                                                                    \
    };

#define DEFINE_DELEGATE_CAST_OP(name, funcname)                               \
    struct name {                                                             \
        template <typename Parent, typename FacadeType, typename Rt>          \
        class accessor : public Parent {                                      \
        protected:                                                            \
            using __signature = photon::FuncSignature<Rt()>;                  \
            Rt (*__funcptr)(void *);                                          \
                                                                              \
        public:                                                               \
            accessor(const accessor &rhs)                                     \
                : Parent(rhs), __funcptr(rhs.__funcptr) {}                    \
            template <typename T, typename = std::enable_if_t<                \
                                      !std::is_base_of<accessor, T>::value>>  \
            accessor(T &obj)                                                  \
                : __funcptr(reinterpret_cast<Rt (*)(void *)>(                 \
                      ::get_member_function_address<T, Rt>(                   \
                          &obj, &T::operator funcname)                        \
                          .f)) {}                                             \
            accessor() : __funcptr(nullptr) {}                                \
            operator funcname() {                                             \
                return __funcptr(photon::DUtils::__get_obj(                   \
                    static_cast<FacadeType &>(*this)));                       \
            }                                                                 \
            using __ParentAccessor = Parent;                                  \
            friend photon::DUtils;                                            \
            template <typename T>                                             \
            friend class photon::FuncSignature;                               \
        };                                                                    \
        template <typename Parent, typename FacadeType>                       \
        class accessor<Parent, FacadeType, void> : public Parent {            \
        protected:                                                            \
            using __signature = photon::FuncSignature<void()>;                \
            void (*__funcptr)(void *);                                        \
                                                                              \
        public:                                                               \
            accessor(const accessor &rhs) : __funcptr(rhs.__funcptr) {}       \
            template <typename T, typename = std::enable_if_t<                \
                                      !std::is_base_of<accessor, T>::value>>  \
            accessor(T &obj)                                                  \
                : __funcptr(reinterpret_cast<void (*)(void *)>(               \
                      ::get_member_function_address(                          \
                          &obj, (void(T::*)()) & T::operator funcname)        \
                          .f)) {}                                             \
            accessor() : __funcptr(nullptr) {}                                \
            operator funcname() {                                             \
                if (__funcptr)                                                \
                    return __funcptr(photon::DUtils::__get_obj(               \
                        static_cast<FacadeType &>(*this)));                   \
                return {};                                                    \
            }                                                                 \
            using __ParentAccessor = Parent;                                  \
            friend photon::DUtils;                                            \
            template <typename T>                                             \
            friend class photon::FuncSignature;                               \
        };                                                                    \
        template <typename Parent, typename FacadeType>                       \
        class accessor<Parent, FacadeType, photon::SelfRef> : public Parent { \
        protected:                                                            \
            using __signature = photon::FuncSignature<photon::SelfRef()>;     \
            void *(*__funcptr)(void *);                                       \
                                                                              \
        public:                                                               \
            accessor(const accessor &rhs) : __funcptr(rhs.__funcptr) {}       \
            template <typename T, typename = std::enable_if_t<                \
                                      !std::is_base_of<accessor, T>::value>>  \
            accessor(T &obj)                                                  \
                : __funcptr((void *(*)(void *))::get_member_function_address( \
                                &obj, &T::operator funcname)                  \
                                .f) {}                                        \
            accessor() : __funcptr(nullptr) {}                                \
            operator funcname() {                                             \
                void *obj = __funcptr(photon::DUtils::__get_obj(              \
                    static_cast<FacadeType &>(*this)));                       \
                FacadeType ret = static_cast<FacadeType &>(*this);            \
                photon::DUtils::__set_obj(ret, obj);                          \
                return ret;                                                   \
            }                                                                 \
            using __ParentAccessor = Parent;                                  \
            friend photon::DUtils;                                            \
            template <typename T>                                             \
            friend class photon::FuncSignature;                               \
        };                                                                    \
    };

#define DEFINE_DELEGATE_OP(name, opname) \
    DEFINE_DELEGATE_FUNCTION(name, operator opname)

// Overload ----- Function type only

template <typename FuncType>
class FuncSignature;

template <typename T, typename Rt, typename... Args>
struct Prototype {
    using type = Rt (T::*)(Args...);
};

template <typename T, typename... Args>
struct Prototype<T, SelfRef, Args...> {
    using type = T &(T::*)(Args...);
};

template <typename... Args>
struct Prototype<void, SelfRef, Args...> {
    using type = void *(*)(void *, Args...);
};

template <typename Rt, typename... Args>
struct Prototype<void, Rt, Args...> {
    using type = Rt (*)(void *, Args...);
};

template <typename Accessor, typename Rt, typename... Args>
struct FuncMatch;

template <typename Rt, typename... Args>
struct FuncMatch<AccessorBase, Rt, Args...> {
    using type = AccessorBase;
};

template <typename Accessor, typename Rt, typename... Args>
struct FuncMatch<Accessor, Rt &, Args...> {
    using type = AccessorBase;
};

template <typename Rt, typename... Args>
class FuncSignature<Rt(Args...)> {
public:
    using Signature = Rt(Args...);
    using RetType = Rt;
    using ArgsType = std::tuple<Args...>;
    using Idx = std::make_integer_sequence<int, sizeof...(Args)>;
    using Dx = Delegate<
        std::conditional_t<std::is_same<SelfRef, Rt>::value, void *, Rt>,
        Args...>;

    template <typename DR>
    struct DCastable : std::false_type {};

    template <typename DR>
    struct DCastable<Delegate<DR &, Args...>> : std::true_type {};

    template <typename T>
    using Prototype = typename Prototype<T, Rt, Args...>::type;

    template <typename Parent, typename Dispatch, typename FacadeType>
    using accessor =
        typename Dispatch::template accessor<Parent, FacadeType, Rt, Args...>;

    template <typename Accessor>
    using Match = std::conditional_t<
        std::is_same<AccessorBase, Accessor>::value, AccessorBase,
        std::conditional_t<(std::is_same<Rt (*)(void *, Args...),
                                         decltype(std::declval<Accessor>()
                                                      .__funcptr)>::value) ||
                               (std::is_same<void *(*)(void *, Args...),
                                             decltype(std::declval<Accessor>()
                                                          .__funcptr)>::value),
                           Accessor, typename Accessor::__ParentAccessor>>;
};

// OverloadGroup ----- Function overloads

namespace Overloads {

template <typename Dispatch, typename FacadeType, typename... Overloads>
struct OverloadGroup;

template <typename Dispatch, typename FacadeType>
struct OverloadGroup<Dispatch, FacadeType> {
    using type = AccessorBase;
};

template <typename Dispatch, typename FacadeType, typename O,
          typename... Overloads>
struct OverloadGroup<Dispatch, FacadeType, O, Overloads...> {
    using type = typename O::template accessor<
        typename OverloadGroup<Dispatch, FacadeType, Overloads...>::type,
        Dispatch, FacadeType>;
};
}  // namespace Overloads

// Feature ----- Dispatch bind to Overloads

namespace DFeature {

template <typename Dispatch, typename... FuncTypes>
struct Feature {
    template <typename FacadeType>
    using accessor =
        typename Overloads::OverloadGroup<Dispatch, FacadeType,
                                          FuncSignature<FuncTypes>...>::type;
};

}  // namespace DFeature

// Delegates ----- Conbine of object pointer, dispatches and accessors

namespace FeatureBuild {
template <typename Dispatch, typename... Features>
struct MatchFeature {};

template <typename Dispatch, typename... FuncTypes>
struct MatchFeature<Dispatch, DFeature::Feature<Dispatch, FuncTypes...>> {
    using type = DFeature::Feature<Dispatch, FuncTypes...>;
};

template <typename Dispatch, typename... FuncTypes, typename... OtherConvs>
struct MatchFeature<Dispatch, DFeature::Feature<Dispatch, FuncTypes...>,
                    OtherConvs...> {
    using type = DFeature::Feature<Dispatch, FuncTypes...>;
};

template <typename Dispatch, typename FirstConv, typename... OtherConvs>
struct MatchFeature<Dispatch, FirstConv, OtherConvs...> {
    using type = typename MatchFeature<Dispatch, OtherConvs...>::type;
};
}  // namespace FeatureBuild
template <typename... Convs>
struct Delegates : public DelegatesBase,
                   public Convs::template accessor<Delegates<Convs...>>... {
    template <typename T, typename = std::enable_if_t<
                              !std::is_base_of<DelegatesBase, T>::value>>
    explicit Delegates(T &obj)
        : DelegatesBase(obj),
          Convs::template accessor<Delegates<Convs...>>(obj)... {}

    Delegates(const Delegates &other)
        : DelegatesBase(other.__objptr),
          Convs::template accessor<Delegates<Convs...>>(
              static_cast<
                  typename Convs::template accessor<Delegates<Convs...>>>(
                  other))... {}

    Delegates() = default;
};

struct DUtils {
    template <typename... Convs>
    static void *__get_obj(Delegates<Convs...> &x) {
        return static_cast<DelegatesBase &>(x).__objptr;
    }
    template <typename... Convs>
    static void *__set_obj(Delegates<Convs...> &x, void *p) {
        return static_cast<DelegatesBase &>(x).__objptr = p;
    }

    template <typename DC, typename O, typename D>
    static std::enable_if_t<!std::is_same<O, AccessorBase>::value, bool>
    __bind_func(D &d, decltype(DC::_func) fptr) {
        ((O &)d).__funcptr = (decltype(((O &)d).__funcptr))fptr;
        return true;
    }

    template <typename DC, typename O, typename D>
    static std::enable_if_t<std::is_same<O, AccessorBase>::value, bool>
    __bind_func(D &d, decltype(DC::_func) fptr) {
        return false;
    }

    template <typename DC, typename O, typename D>
    static std::enable_if_t<!std::is_same<O, AccessorBase>::value, DC>
    __get_func(D &d) {
        return DC(d.__objptr, (decltype(DC::_func))((O &)d).__funcptr);
    }

    template <typename DC, typename O, typename D>
    static std::enable_if_t<std::is_same<O, AccessorBase>::value, DC>
    __get_func(D &d) {
        return DC();
    }

    template <typename D, typename C, typename... Convs>
    static bool bind_func_delegate(Delegates<Convs...> &d,
                                   typename FuncSignature<C>::Dx fptr) {
        using FS = FuncSignature<C>;
        using O =
            typename FS::template Match<typename FeatureBuild::MatchFeature<
                D, Convs...>::type::template accessor<Delegates<Convs...>>>;
        using DC = typename FS::Dx;

        if (!d.__objptr) d.__objptr = fptr._obj;
        if (d.__objptr && fptr._obj && fptr._obj != d.__objptr) return false;
        return __bind_func<DC, O>(d, fptr._func);
    }

    template <typename T, typename Ret, typename... Args>
    static Delegate<Ret, Args...> __build_delegate(T *obj,
                                                   Ret (T::*func)(Args...)) {
        return {obj, func};
    }

    template <typename D, typename C, typename T, typename Dlgs>
    static bool bind_func(
        Dlgs &d, T *obj,
        typename FuncSignature<C>::template Prototype<T> func) {
        using FS = FuncSignature<C>;
        using DC = typename FS::Dx;
        auto ret = __build_delegate(obj, func);
        return bind_func_delegate<D, C>(d, *reinterpret_cast<DC *>(&ret));
    }

    template <typename D, typename C, typename Dlgs>
    static bool bind_func(
        Dlgs &d, void *obj,
        typename FuncSignature<C>::template Prototype<void> func) {
        using FS = FuncSignature<C>;
        using DC = typename FS::Dx;
        return bind_func_delegate<D, C>(d, DC(obj, func));
    }

    template <typename D, typename C, typename Dlgs>
    static bool bind_func(
        Dlgs &d, std::nullptr_t,
        typename FuncSignature<C>::template Prototype<void> func) {
        using FS = FuncSignature<C>;
        using DC = typename FS::Dx;
        return bind_func_delegate<D, C>(d,
                                        DC(nullptr, (decltype(DC::_func))func));
    }

    template <typename D, typename F>
    struct __match_og;

    template <typename D, typename... Convs>
    struct __match_og<D, Delegates<Convs...>> {
        using OG = typename FeatureBuild::MatchFeature<
            D, Convs...>::type::template accessor<Delegates<Convs...>>;
        using FS = typename OG::__signature;
        using C = typename FS::Signature;
        using DC = typename FS::Dx;
    };

    template <typename D, typename T, typename Dlgs, typename = void(T::*)()>
    static bool bind_func(
        Dlgs &d, T *obj,
        typename __match_og<D, Dlgs>::FS::template Prototype<T> func) {
        using OG = __match_og<D, Dlgs>;
        auto ret = __build_delegate(obj, func);
        return bind_func_delegate<D, typename OG::C>(
            d, *reinterpret_cast<typename OG::DC *>(&ret));
    }

    template <typename D, typename Dlgs>
    static bool bind_func(
        Dlgs &d, void *obj,
        typename __match_og<D, Dlgs>::FS::template Prototype<void> func) {
        using OG = __match_og<D, Dlgs>;
        return bind_func_delegate<D, typename OG::C>(d, DC(obj, func));
    }

    template <typename D, typename Dlgs>
    static bool bind_func(
        Dlgs &d, std::nullptr_t,
        typename __match_og<D, Dlgs>::FS::template Prototype<void> func) {
        using OG = __match_og<D, Dlgs>;
        return bind_func_delegate<D, typename OG::C>(
            d, DC(nullptr, (decltype(OG::DC::_func))func));
    }

    template <typename T, typename... Convs>
    static bool bind_obj(Delegates<Convs...> &d, T *obj) {
        if (d.__objptr) return false;
        d.__objptr = obj;
        return true;
    }

    template <typename T, typename... Convs>
    static bool bind_obj_and_func(Delegates<Convs...> &d, T &obj) {
        d = Delegates<Convs...>(obj);
        return true;
    }

    template <typename D, typename C, typename... Convs>
    static auto get_func(Delegates<Convs...> &d) {
        using FS = FuncSignature<C>;
        using O =
            typename FS::template Match<typename FeatureBuild::MatchFeature<
                D, Convs...>::type::template accessor<Delegates<Convs...>>>;
        using DC = typename FS::Dx;

        return __get_func<DC, O>(d);
    }

    template <typename D, typename Dlg>
    static auto get_func(Dlg &d) {
        using OG = __match_og<D, Dlg>;
        return __get_func<typename OG::DC, typename OG::OG>(d);
    }
};

// Pre-defined operators

namespace DFeature {

DEFINE_DELEGATE_OP(EQ, ==);
DEFINE_DELEGATE_OP(NE, !=);
DEFINE_DELEGATE_OP(LT, <);
DEFINE_DELEGATE_OP(LE, <=);
DEFINE_DELEGATE_OP(GT, >);
DEFINE_DELEGATE_OP(GE, >=);

DEFINE_DELEGATE_OP(OR, |);
DEFINE_DELEGATE_OP(AND, &);
DEFINE_DELEGATE_OP(XOR, ^);
DEFINE_DELEGATE_UNARY_OP(NOT, ~);
DEFINE_DELEGATE_UNARY_OP(LNOT, !);
DEFINE_DELEGATE_OP(LOR, ||);
DEFINE_DELEGATE_OP(LAND, &&);

DEFINE_DELEGATE_OP(ADD, +);
DEFINE_DELEGATE_OP(SUB, -);
DEFINE_DELEGATE_OP(MUL, *);
DEFINE_DELEGATE_OP(DIV, /);
DEFINE_DELEGATE_OP(MOD, %);

DEFINE_DELEGATE_OP(ADD_ASSIGN, +=);
DEFINE_DELEGATE_OP(SUB_ASSIGN, -=);
DEFINE_DELEGATE_OP(MUL_ASSIGN, *=);
DEFINE_DELEGATE_OP(DIV_ASSIGN, /=);
DEFINE_DELEGATE_OP(MOD_ASSIGN, %=);
DEFINE_DELEGATE_OP(OR_ASSIGN, |=);
DEFINE_DELEGATE_OP(AND_ASSIGN, &=);
DEFINE_DELEGATE_OP(XOR_ASSIGN, ^=);
DEFINE_DELEGATE_OP(LSH, <<);
DEFINE_DELEGATE_OP(RSH, >>);
DEFINE_DELEGATE_OP(LSH_ASSIGN, <<=);
DEFINE_DELEGATE_OP(RSH_ASSIGN, >>=);

DEFINE_DELEGATE_OP(ASSIGN, =);
DEFINE_DELEGATE_OP(INVOKE, ());
DEFINE_DELEGATE_OP(INC, ++);
DEFINE_DELEGATE_OP(DEC, --);

}  // namespace DFeature

}  // namespace photon

#pragma GCC diagnostic pop