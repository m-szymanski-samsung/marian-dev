// Simple logging-objects container for the scheduler.h, developed during MT Marathon 2019

#pragma once

#include <string>
#include <utility>
#include <vector>
#include <sstream>
#include "common/definitions.h"
#include "common/logging.h"
#include "spdlog/include/spdlog/fmt/bundled/format.h"

namespace marian {

/// Abstract logged-object
class ILoggedStat {
public:
    virtual void reset() = 0;
    virtual const std::string& label() const = 0;
    virtual const std::string& unit() const = 0;
    virtual std::string formatValue() const = 0;
};

/// A typical base for logging object; still abstract: requires value formatting implementation
class LoggedStatAbstractBase : public ILoggedStat {
    const std::string label_, unit_;
public:
    LoggedStatAbstractBase(std::string label, std::string unit)
    : label_(std::move(label)), unit_(std::move(unit)) {}
    void reset() override {}
    std::string print() const { return prettyPrint(*this); }
    const std::string& label() const override { return label_; }
    const std::string& unit() const override { return unit_; }
    static std::string prettyPrint(const ILoggedStat &obj) {
        std::string ret(obj.label().empty()?std::string():obj.label()+" ");
        ret += obj.formatValue(); // template-method pattern
        return ret + (obj.unit().empty()?std::string():obj.unit());
    }
};

/// A typical lambda-based implementation of a logged-object
template <class FormatFunc>
class FormatLambdaStat : public LoggedStatAbstractBase {
    FormatFunc formatFn_;
public:
    FormatLambdaStat(const std::string &label, FormatFunc formatFn, const std::string &unit)
    : LoggedStatAbstractBase(label, unit), formatFn_(formatFn) {}
    std::string formatValue() const override {
        return formatFn_();
    }
};
template <class FormatFunc, class ResetFunc>
class TwoLambdasStat : public FormatLambdaStat<FormatFunc> {
    ResetFunc resetFn_;
public:
    TwoLambdasStat(const std::string &label, FormatFunc formatFn, const std::string &unit, ResetFunc resetFn)
    : FormatLambdaStat<FormatFunc>(label,formatFn,unit), resetFn_(resetFn) {}
    void reset() override {
        resetFn_();
    }
};
/// Helper LambdaStat makers
template <class FormatFunc>
Ptr<FormatLambdaStat<FormatFunc>> makeLambdaStat(const std::string &label, FormatFunc formatFn, const std::string &unit=std::string()) { return New<FormatLambdaStat<FormatFunc>>(label,formatFn,unit); }
template <class FormatFunc, class ResetFunc>
Ptr<TwoLambdasStat<FormatFunc,ResetFunc>> makeLambdaStat(const std::string &label, FormatFunc formatFn, const std::string &unit, ResetFunc resetFn) { return New<TwoLambdasStat<FormatFunc,ResetFunc>>(label,formatFn,unit,resetFn); }

/// The container of logged-objects
class LoggingContainer {
    template<class Ty>
    static auto formatLambda(const std::string &format, Ty *p)
#if __cplusplus < 201402L
     -> std::function<std::string()> // this is just to make pre-C++14 compiler happy
#endif
    { return [format,p](){ return fmt::format(format, *p); }; }

    template<class GetterFunc>
    static auto getterLambda(const std::string &format, GetterFunc getterFn)
#if __cplusplus < 201402L
     -> std::function<std::string()> // this is just to make pre-C++14 compiler happy
#endif
    { return [format,getterFn](){return fmt::format(format,getterFn());}; }

    template<class Ty>
    static auto resetLambda(Ty*p, Ty resetVal)
#if __cplusplus < 201402L
     -> std::function<void(void)>  // this is just to make pre-C++14 compiler happy
#endif
    { return [p,resetVal](){ *p = resetVal; }; }
protected:
    std::vector<Ptr<ILoggedStat>> loggedstats_;
public:
    LoggingContainer(std::initializer_list<Ptr<ILoggedStat>> initList={}) : loggedstats_(initList) {}
    void addObject(Ptr<ILoggedStat> s) {
        loggedstats_.push_back(s);
    }
    template <class FormatFunc>
    void addLambda(const std::string &label, FormatFunc formatFn, const std::string &unit=std::string()) {
        addObject(makeLambdaStat(label,formatFn,unit));
    }
    template <class FormatFunc, class ResetFunc>
    void addLambda(const std::string &label, FormatFunc formatFn, const std::string &unit, ResetFunc resetFn) {
        addObject(makeLambdaStat(label,formatFn,unit,resetFn));
    }
    template <class GetterFunc>
    void addLambda(const std::string &label, const std::string &format, GetterFunc getterFn, const std::string &unit=std::string()) {
        addObject(makeLambdaStat(label,getterLambda(format,getterFn),unit));
    }
    template <class GetterFunc, class ResetFunc>
    void addLambda(const std::string &label, const std::string &format, GetterFunc getterFn, const std::string &unit, ResetFunc resetFn) {
        addObject(makeLambdaStat(label,getterLambda(format,getterFn),unit,resetFn));
    }
    template<class Ty>
    void addVariable(const std::string &label, const std::string &format, Ty &var, const std::string &unit=std::string()) {  // caution: storing pointer -- &var must not be a temporary!
        addLambda(label, formatLambda(format,&var), unit);
    }
    template<class Ty>
    void addVariable(const std::string &label, const std::string &format, Ty &var, const std::string &unit, const Ty resetVal) {  // caution: storing pointer -- &var must not be a temporary!
        addLambda(label, formatLambda(format,&var), unit, resetLambda(&var,resetVal));
    }
    template<class Ty>
    void addVariable(const std::string &label, Ty &var, const std::string &unit=std::string()) {  // caution: storing pointer -- &var must not be a temporary!
        addVariable(label, "{}", var, unit);
    }
    template<class Ty>
    void addVariable(const std::string &label, Ty &var, const std::string &unit, const Ty resetVal) {  // caution: storing pointer -- &var must not be a temporary!
        addVariable(label, "{}", var, unit, resetVal);
    }
    void logInfo() const {
        std::ostringstream str;
        if (!loggedstats_.empty()) str << LoggedStatAbstractBase::prettyPrint(*loggedstats_.front());
        for (size_t im=1; im<loggedstats_.size(); im++) str << " : " << LoggedStatAbstractBase::prettyPrint(*loggedstats_[im]);
        LOG(info, str.str());
    }
    std::map<std::string,std::string> toMap() const { // TODO: decide what to do with duplicates and empty label&unit objects...
        std::map<std::string,std::string> logMap;
        for (auto &o : loggedstats_) {
            auto lab = o->label();
            auto uni = o->unit();
            if (!uni.empty()) uni = (lab.empty()?"[":" [") + uni + "]";
            logMap[lab + uni] = o->formatValue();
        }
        return logMap;
    }
    void resetAll() {
        for (auto s : loggedstats_)
            s->reset();
    }
}; // class LoggingContainer

}; // namespace marian
