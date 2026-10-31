// Four-function calculator engine with standard precedence (x and / bind tighter).
#pragma once

#include <string>

namespace core {

class Calculator {
public:
    enum class Op { None, Add, Sub, Mul, Div };

    void digit(int d);
    void decimal_point();
    void op(Op o);
    void equals();
    void clear();          // AC: everything
    void clear_entry();    // C: only the number being typed; the pending operation stays
    // True when the clear key should read "C" (an entry or error to clear) rather than "AC".
    bool has_entry() const { return error_ || (typing_ && entry_ != "0" && entry_ != "-0"); }
    void toggle_sign();
    void percent();
    void backspace();

    std::string display() const;
    bool error() const { return error_; }

private:
    double current() const;
    void set_current(double v);
    void apply_pending_high();
    void resolve_all();

    std::string entry_ = "0";  // digits being typed
    bool typing_ = false;
    double low_acc_ = 0;       // accumulator for + / -
    Op low_op_ = Op::None;
    double high_acc_ = 0;      // accumulator for x / /
    Op high_op_ = Op::None;
    double value_ = 0;         // last result when not typing
    bool error_ = false;
};

}  // namespace core
