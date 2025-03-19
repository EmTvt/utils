#include "shared_ptr.h"
#include <utility>

struct A : enable_shared_from_this<A>{
    A() {
        cout << "A" << endl;
    }
    void test() {
        shared_from_this()->print();
    }
    void print() {
        cout << "print" << endl;
    }
    ~A() {
        cout << "~A" << endl;
    }
};


int main() {
    shared_ptr<A> p1(new A{});
    shared_ptr<A> p2 = make_shared<A>();
    p1->print();
    p2->print();
}
