#pragma once

#include <cstddef>

struct DummyDeleter {
    template <typename T>
    void operator()(const T&) {
        // dummy
    }
};

template <typename Member, typename MemberHash>
class PtrMemberHash {
public:
    PtrMemberHash(MemberHash mh)
        : mh_(mh) {
    }

    PtrMemberHash(): PtrMemberHash(MemberHash()) {
    }

    size_t operator()(const Member* pm) {
        return mh_(*pm);
    }

private:
    MemberHash mh_;
};

template <typename Member, typename MemberCompare>
class PtrMemberEqual {
public:
    PtrMemberEqual(MemberCompare mc)
        : mc_(mc) {
    }

    PtrMemberEqual(): PtrMemberEqual(MemberCompare()) {
    }

    bool operator()(const Member* pm1, const Member* pm2) {
        return (pm1 == pm2) || !(mc_(*pm1, *pm2) || mc_(*pm2, *pm1));
    }

private:
    MemberCompare mc_;
};
