// Simple logging-objects container for the scheduler.h, developed during MT Marathon 2019

#pragma once

#include <string>
#include <vector>
#include <sstream>
#include "common/definitions.h"
#include "common/logging.h"
#include "spdlog/include/spdlog/fmt/bundled/format.h"

namespace marian {

/// Abstract logged-object
class LoggedStat {
    const std::string label_, unit_;
public:
    LoggedStat(const std::string &label, const std::string &unit) 
    : label_(label), unit_(unit) {}
    virtual void reset() {}
    virtual std::string format_value() const = 0;
    std::string pretty_print() const {
        std::string ret(label_.empty()?std::string():label_+" ");
        ret += format_value();
        return ret + (unit_.empty()?std::string():unit_);
    }
    const std::string& label() const { return label_; }
    const std::string& unit() const { return unit_; }
};

/// A typical lambda-based implementation of a logged-object
template <class FormatFunc>
class FormatLambdaStat : public LoggedStat {
    FormatFunc format_fn_;
public:
    FormatLambdaStat(const std::string &label, FormatFunc format_fn, const std::string &unit) 
    : LoggedStat(label,unit), format_fn_(format_fn) {}
    virtual std::string format_value() const override {
        return format_fn_();
    }
};
template <class FormatFunc, class ResetFunc>
class TwoLambdasStat : public FormatLambdaStat<FormatFunc> {
    ResetFunc reset_fn_;
public:
    TwoLambdasStat(const std::string &label, FormatFunc format_fn, const std::string &unit, ResetFunc reset_fn) 
    : FormatLambdaStat<FormatFunc>(label,format_fn,unit), reset_fn_(reset_fn) {}
    virtual void reset() override {
        reset_fn_();
    }
};
template <class FormatFunc>
Ptr<FormatLambdaStat<FormatFunc>> makeLambdaStat(const std::string &label, FormatFunc format_fn, const std::string &unit=std::string()) { return New<FormatLambdaStat<FormatFunc>>(label,format_fn,unit); }
template <class FormatFunc, class ResetFunc>
Ptr<TwoLambdasStat<FormatFunc,ResetFunc>> makeLambdaStat(const std::string &label, FormatFunc format_fn, const std::string &unit, ResetFunc reset_fn) { return New<TwoLambdasStat<FormatFunc,ResetFunc>>(label,format_fn,unit,reset_fn); }

/// The container of logged-objects
class LoggingContainer {
    template<class Ty>
    static auto format_lambda(const std::string &format, Ty *p) 
     -> std::function<std::string()> // this is just to make pre-C++14 compiler happy
    {
        return [format,p](){ return fmt::format(format, *p); };
    }
    template<class GetterFunc>
    static auto getter_lambda(const std::string &format, GetterFunc getter_fn)
     -> std::function<std::string()> // this is just to make pre-C++14 compiler happy
    {
        return [format,getter_fn](){return fmt::format(format,getter_fn());};
    }
    template<class Ty>
    static auto reset_lambda(Ty*p, Ty reset_val)
     -> std::function<void(void)>  // this is just to make pre-C++14 compiler happy
    {
        return [p,reset_val](){ *p = reset_val; };
    }
protected:
    std::vector<Ptr<LoggedStat>> loggedstats_;
public:
    LoggingContainer(std::initializer_list<Ptr<LoggedStat>> init_list={}) : loggedstats_(init_list) {}
    void add_object(Ptr<LoggedStat> s) {
        loggedstats_.push_back(s);
    }
    template <class FormatFunc>
    void add_lambda(const std::string &label, FormatFunc format_fn, const std::string &unit=std::string()) {
        add_object(makeLambdaStat(label,format_fn,unit));
    }
    template <class FormatFunc, class ResetFunc>
    void add_lambda(const std::string &label, FormatFunc format_fn, const std::string &unit, ResetFunc reset_fn) {
        add_object(makeLambdaStat(label,format_fn,unit,reset_fn));
    }
    template <class GetterFunc>
    void add_lambda(const std::string &label, const std::string &format, GetterFunc getter_fn, const std::string &unit=std::string()) {
        add_object(makeLambdaStat(label,getter_lambda(format,getter_fn),unit));
    }
    template <class GetterFunc, class ResetFunc>
    void add_lambda(const std::string &label, const std::string &format, GetterFunc getter_fn, const std::string &unit, ResetFunc reset_fn) {
        add_object(makeLambdaStat(label,getter_lambda(format,getter_fn),unit,reset_fn));
    }
    template<class Ty>
    void add_variable(const std::string &label, const std::string &format, Ty &var, const std::string &unit=std::string()) {  // caution: storing pointer -- &var must not be a temporary!
        add_lambda(label, format_lambda(format,&var), unit);
    }
    template<class Ty>
    void add_variable(const std::string &label, const std::string &format, Ty &var, const std::string &unit, const Ty reset_val) {  // caution: storing pointer -- &var must not be a temporary!
        add_lambda(label, format_lambda(format,&var), unit, reset_lambda(&var,reset_val));
    }
    template<class Ty>
    void add_variable(const std::string &label, Ty &var, const std::string &unit=std::string()) {  // caution: storing pointer -- &var must not be a temporary!
        add_variable(label, "{}", var, unit);
    }
    template<class Ty>
    void add_variable(const std::string &label, Ty &var, const std::string &unit, const Ty reset_val) {  // caution: storing pointer -- &var must not be a temporary!
        add_variable(label, "{}", var, unit, reset_val);
    }
    void log_info() const {
        std::ostringstream str;
        if (!loggedstats_.empty()) str << loggedstats_.front()->pretty_print();
        for (size_t im=1; im<loggedstats_.size(); im++) str << " : " << loggedstats_[im]->pretty_print();
        LOG(info, str.str());
    }
    std::map<std::string,std::string> to_map() const { // TODO: decide what to do with duplicates and empty label&unit objects...
        std::map<std::string,std::string> log_map;
        for (auto &o : loggedstats_) {
            auto lab = o->label();
            auto uni = o->unit();
            if (!uni.empty()) uni = (lab.empty()?"[":" [") + uni + "]";
            log_map[lab+uni] = o->format_value();
        }
        return log_map;
    }
    void reset_all() {
        for (auto s : loggedstats_)
            s->reset();
    }
};


};
