#include "core/calculator.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace core {

namespace {
double apply(double a, Calculator::Op op, double b, bool& err) {
    switch (op) {
        case Calculator::Op::Add: return a + b;
        case Calculator::Op::Sub: return a - b;
        case Calculator::Op::Mul: return a * b;
        case Calculator::Op::Div:
            if (b == 0) {
                err = true;
                return 0;
            }
            return a / b;
        default: return b;
    }
}
bool is_high(Calculator::Op o) { return o == Calculator::Op::Mul || o == Calculator::Op::Div; }
}  // namespace

double Calculator::current() const { return typing_ ? std::strtod(entry_.c_str(), nullptr) : value_; }

void Calculator::set_current(double v) {
    value_ = v;
    typing_ = false;
    if (!std::isfinite(v)) error_ = true;
}

void Calculator::digit(int d) {
    if (error_) clear();
    if (!typing_) {
        entry_ = "0";
        typing_ = true;
    }
    size_t digits = 0;
    for (char c : entry_) digits += (c >= '0' && c <= '9');
    if (digits >= 9) return;
    if (entry_ == "0")
        entry_ = std::to_string(d);
    else if (entry_ == "-0")
        entry_ = "-" + std::to_string(d);
    else
        entry_ += static_cast<char>('0' + d);
}

void Calculator::decimal_point() {
    if (error_) clear();
    if (!typing_) {
        entry_ = "0";
        typing_ = true;
    }
    if (entry_.find('.') == std::string::npos) entry_ += '.';
}

void Calculator::apply_pending_high() {
    if (high_op_ != Op::None) {
        set_current(apply(high_acc_, high_op_, current(), error_));
        high_op_ = Op::None;
    }
}

void Calculator::resolve_all() {
    apply_pending_high();
    if (low_op_ != Op::None) {
        set_current(apply(low_acc_, low_op_, current(), error_));
        low_op_ = Op::None;
    }
}

void Calculator::op(Op o) {
    if (error_) return;
    if (is_high(o)) {
        apply_pending_high();
        high_acc_ = current();
        high_op_ = o;
    } else {
        resolve_all();
        low_acc_ = current();
        low_op_ = o;
    }
    value_ = current();
    typing_ = false;
}

void Calculator::equals() {
    if (error_) return;
    resolve_all();
    set_current(current());
}

void Calculator::clear() {
    *this = Calculator{};
}

void Calculator::clear_entry() {
    if (error_) return clear();
    entry_ = "0";
    typing_ = true;
}

void Calculator::toggle_sign() {
    if (error_) return;
    if (typing_) {
        entry_ = entry_[0] == '-' ? entry_.substr(1) : "-" + entry_;
    } else {
        set_current(-value_);
    }
}

void Calculator::percent() {
    if (error_) return;
    double v = current() / 100.0;
    // "50 + 10%" means 10% of 50, as on common calculators.
    if (low_op_ == Op::Add || low_op_ == Op::Sub) v *= low_acc_;
    set_current(v);
}

void Calculator::backspace() {
    if (!typing_) return;
    entry_.pop_back();
    if (entry_.empty() || entry_ == "-") entry_ = "0";
}

std::string Calculator::display() const {
    if (error_) return "Error";
    if (typing_) return entry_;
    double v = value_;
    if (v == 0) return "0";
    char buf[32];
    double av = std::fabs(v);
    if (av >= 1e9 || av < 1e-6)
        std::snprintf(buf, sizeof buf, "%.4e", v);
    else
        std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

}  // namespace core
