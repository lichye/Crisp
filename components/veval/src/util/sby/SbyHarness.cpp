#include "SbyHarness.h"
#include "util/ReadGeneratedSva.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace veval::verification {
size_t balancedEnd(const std::string &s, size_t start) {
    if (start >= s.size() || s[start] != '(') throw Unsupported("missing opening parenthesis");
    unsigned depth = 0;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        if (s[i] == ')' && !--depth) return i;
    }
    throw Unsupported("unbalanced parentheses");
}
std::string stripParens(std::string s) {
    s = trim(s);
    while (!s.empty() && s[0] == '(' && balancedEnd(s, 0) == s.size() - 1) s = trim(s.substr(1,s.size()-2));
    return s;
}
static std::string join(const std::vector<std::string> &v, const std::string &sep) {
    std::string out;
    for (const auto &s : v) { if (!out.empty()) out += sep; out += s; }
    return out;
}
static std::string parenthesized(const std::vector<std::string> &v) {
    std::vector<std::string> out; for (const auto &s : v) out.push_back("(" + s + ")"); return join(out," || ");
}
SbyProduct::SbyProduct(const fs::path &p) : path(p) {
    const auto content = read(p); std::smatch m;
    for (const auto& [name,definition]:util::generatedFunctions(content))
        functions.emplace(name,definition.text);
    const auto definitions = util::generatedProperties(content);
    static const std::regex moduleRe(R"((?:^|\n)module\s+([A-Za-z_]\w*)\s*\()"),
        instanceRe(R"((?:^|\n)// instance:\s*(\S+))"),
        propertyRe(R"((\w+):\s*(assert|assume)\s+property\s*\()"),
        portRe(R"(^(input|output|inout)\s+(?:logic|wire|reg)?\s*(?:signed\s+)?(?:\[(\d+):(\d+)\])?\s*(\w+)$)"),
        signedRe(R"(\bsigned\b)");
    if (std::regex_search(content,m,instanceRe)) instance = m[1];
    if (!std::regex_search(content,m,moduleRe)) throw Unsupported("no product module header");
    module = m[1]; const size_t begin = m.position() + m.length() - 1, end = balancedEnd(content,begin);
    std::istringstream header(content.substr(begin,end-begin));
    for (std::string line; std::getline(header,line);) {
        line = trim(line.substr(0,line.find("//")));
        if (!line.empty() && line.back() == ',') line.pop_back();
        if (std::regex_match(line,m,portRe)) {
            const auto width = m[2].matched ? std::abs(std::stoi(m[2])-std::stoi(m[3]))+1 : 1;
            ports[m[4]] = width;
            if (std::regex_search(line,signedRe)) signedPorts.insert(m[4]);
        }
    }
    static const std::regex tieRe(R"(^\s*wire\s+(?:signed\s+)?(?:\[(\d+):(\d+)\]\s*)?(\w+)\s*=\s*([^;]+);)");
    {
        std::istringstream body(content.substr(end)); std::string line;
        while (std::getline(body,line) && line.find(" u_dut(") == line.npos)
            if (std::regex_search(line,m,tieRe))
                tiedWires[m[3]] = m[1].matched ? std::abs(std::stoi(m[1])-std::stoi(m[2]))+1 : 1;
    }
    size_t first = std::string::npos;
    for (std::sregex_iterator i(content.begin(),content.end(),propertyRe), e; i != e; ++i) {
        const auto &match = *i;
        size_t open = match.position()+match.length()-1, close = balancedEnd(content,open);
        if (first == std::string::npos) first = match.position();
        auto body = spaces(content.substr(open+1,close-open-1));
        std::string declarations;
        static const std::regex reference(R"(^(@\s*\(\s*posedge\s+\w+\))\s*(__cf_rule_\w+)$)");
        std::smatch named;
        if (std::regex_match(body,named,reference)) {
            const auto definition = definitions.find(named[2]);
            if (definition == definitions.end()) throw Unsupported("named product property has no definition");
            if (match[2] != "assert") throw Unsupported("local variables in assumption: " + match[1].str());
            declarations = definition->second.declarations;
            body = named[1].str() + " " + definition->second.body;
        }
        J block=object({{"label",str(match[1])},{"kind",str(match[2])},{"body",str(body)}});
        if (!declarations.empty()) put(block,"local_declarations",str(declarations));
        append(blocks,block);
    }
    std::istringstream lines(content);
    for (std::string line; std::getline(lines,line);) {
        auto at = line.find("premise:");
        if (at != line.npos) append(premises,str(trim(line.substr(at+8))));
        if (line.rfind("// observation:",0)==0) append(observations,parse(line.substr(15)));
        if (line.rfind("// sampling:",0)==0) {
            if (!sampling.is_null()) throw Unsupported("product states two sampling records");
            sampling = parse(line.substr(12));
        }
    }
    if (first == std::string::npos) first = content.rfind("endmodule");
    if (first == std::string::npos) throw Unsupported("no endmodule");
    prelude = stripTrailingComments(util::withoutGeneratedFunctions(
        util::withoutGeneratedProperties(content.substr(0,first))));
}
// Scan backward instead of using a recursive regex on potentially large tails.
// Preserve the first line and blank-only tails; remove tails containing comments.
std::string stripTrailingComments(const std::string &text) {
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    size_t cut = std::string::npos, end = text.size();
    bool comment = false;
    while (end > 0) {
        const size_t newline = text.rfind('\n', end - 1);
        if (newline == std::string::npos) break;
        size_t at = newline + 1;
        while (at < end && space(text[at])) ++at;
        if (at < end) {
            if (end - at < 2 || text.compare(at, 2, "//") != 0) break;
            comment = true;
        }
        cut = newline;
        end = newline;
    }
    if (cut == std::string::npos || !comment) return text;
    return text.substr(0, cut) + "\n";
}
std::string samplingClock(const SbyProduct& product, const std::string& body) {
    static const std::regex event(R"(^@\s*\(\s*posedge\s+([A-Za-z_][A-Za-z_0-9$]*)\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, event) || body.find('@', match.length()) != body.npos)
        throw Unsupported("requires one explicit posedge port event");
    const auto name = match[1].str();
    const auto port = product.ports.find(name);
    if (port == product.ports.end() || port->second != 1)
        throw Unsupported("sampling clock is not a declared one-bit port: " + name);
    return name;
}
struct Token { size_t begin, end; std::string word; };
static std::vector<Token> tokens(const std::string &s) {
    static const std::regex re(R"([()]|\b(?:implies|or|and|not|always)\b)");
    int depth = 0; std::vector<Token> out;
    for (std::sregex_iterator i(s.begin(),s.end(),re),e; i!=e; ++i) {
        auto word = i->str();
        if (word=="(") ++depth; else if (word==")") --depth;
        else if (!depth) out.push_back({size_t(i->position()),size_t(i->position()+i->length()),word});
    }
    return out;
}
static bool has(const std::vector<Token> &ts,const std::string &s) {
    return std::any_of(ts.begin(),ts.end(),[&](const auto &t){return t.word==s;});
}
static std::vector<std::string> split(const std::string &s,const std::vector<Token> &ts,const std::string &word) {
    std::vector<std::string> out; size_t last=0;
    for (const auto &t:ts) if (t.word==word) {out.push_back(trim(s.substr(last,t.begin-last)));last=t.end;}
    out.push_back(trim(s.substr(last)));return out;
}
static std::string booleanTerm(std::string s) {
    s=stripParens(s); const auto ts=tokens(s);
    if (ts.empty()) {
        static const std::regex bad(R"(\b(s_eventually|eventually|s_until(_with)?|until(_with)?|nexttime|s_nexttime|throughout|within|intersect|first_match|iff|disable)\b|\$(stable|rose|fell|changed|onehot0?|countones)\b|##|\|->|\|=>|\[\*|\[->|\[=)");
        if (std::regex_search(s,bad)) throw Unsupported("temporal expression needs native SVA compiler: "+s);
        if (s.empty()) throw Unsupported("empty Boolean term");
        return s;
    }
    if (has(ts,"always")) throw Unsupported("temporal operator inside Boolean term");
    if (has(ts,"implies")) for (const auto &t:ts) if(t.word=="implies")
        return "(!("+booleanTerm(s.substr(0,t.begin))+")) || ("+booleanTerm(s.substr(t.end))+")";
    for(const auto &kind:{"or","and"}) if(has(ts,kind)) {
        std::vector<std::string> out;
        for(const auto &part:split(s,ts,kind)) out.push_back("("+booleanTerm(part)+")");
        return join(out,std::string(kind)=="or"?" || ":" && ");
    }
    if(ts.size()==1 && ts[0].word=="not" && ts[0].begin==0) return "!("+booleanTerm(s.substr(ts[0].end))+")";
    throw Unsupported("unparsed SVA term: "+s);
}
struct Term { bool next; unsigned delay; std::string body; };
static std::vector<Term> flatten(std::string s) {
    s=stripParens(s); const auto ts=tokens(s); std::vector<Term> out;
    if(has(ts,"or")) {
        for(const auto &part:split(s,ts,"or")) {auto child=flatten(part);out.insert(out.end(),child.begin(),child.end());}
        return out;
    }
    // Match the prefix only: a regex over a large consequent can exhaust the stack.
    std::smatch m; static const std::regex window(R"(^always\s*\[\s*(\d+)\s*:\s*(\d+)\s*\]\s*)");
    if(std::regex_search(s,m,window)) {
        auto lo=std::stoul(m[1]),hi=std::stoul(m[2]);
        if(lo!=hi || hi>1024) throw Unsupported("non-point or excessive always window");
        auto inner=stripParens(s.substr(m.length()));
        if(has(tokens(inner),"always")) throw Unsupported("nested always window");
        return {{true,unsigned(lo),inner}};
    }
    if(has(ts,"always")) throw Unsupported("unhandled temporal shape");
    return {{false,0,s}};
}
static void declared(const std::string &expr, const std::set<std::string> &names) {
    static const std::regex literal(R"(\d*'[sS]?[bodhBODH][0-9a-fA-FxXzZ?_]+)"),
        hierarchical(R"([A-Za-z_][A-Za-z_0-9$]*(?:\s*\[[^\]]*\])*\s*\.\s*[A-Za-z_])"),
        integer(R"(\b\d[\d_]*\b)"),
        qualified(R"([A-Za-z_][A-Za-z_0-9$]*\s*::\s*[A-Za-z_][A-Za-z_0-9$]*)"),
        signCast(R"(\b(signed|unsigned)\s*'\s*\()"),
        signFunction(R"(\$(signed|unsigned)\s*\()"),
        name(R"([A-Za-z_][A-Za-z_0-9$]*)");
    auto s=std::regex_replace(expr,literal," ");
    if(std::regex_search(s,hierarchical))
        throw Unsupported("hierarchical name needs observation binding");
    s=std::regex_replace(s,integer," ");
    s=std::regex_replace(s,qualified," ");
    s=std::regex_replace(s,signCast,"(");
    s=std::regex_replace(s,signFunction,"(");
    for(std::sregex_iterator i(s.begin(),s.end(),name),e;i!=e;++i)
        if(!names.count(i->str())) throw Unsupported("undeclared name: "+i->str());
}
struct Encoder {
    const SbyProduct &product;
    bool multiclock=false;
    std::string clock="clk";
    Encoding all;
    std::map<std::pair<std::string,unsigned>,std::string> past;
    unsigned delays=0;
    unsigned width(const std::string &s) {
        std::smatch m;
        if(multiclock && s.size()>2 && s.front()=='{' && s.back()=='}') {
            unsigned total=0;int nested=0;size_t start=1;
            for(size_t i=1;i<s.size();++i) {
                if(s[i]=='{'||s[i]=='('||s[i]=='[')++nested;
                if(s[i]=='}'||s[i]==')'||s[i]==']')--nested;
                if((s[i]==','&&nested==0)||i==s.size()-1) {
                    total+=width(trim(s.substr(start,i-start)));start=i+1;
                }
            }
            if(!total)throw Unsupported("empty history concatenation");
            return total;
        }
        static const std::regex slice(R"(^(\w+)\s*\[\s*(\d+)\s*:\s*(\d+)\s*\]$)"),
            bit(R"(^(\w+)\s*\[\s*\d+\s*\]$)");
        if(std::regex_match(s,m,slice) && product.ports.count(m[1]))
            return std::abs(std::stoi(m[2])-std::stoi(m[3]))+1;
        if(std::regex_match(s,m,bit) && product.ports.count(m[1])) return 1;
        auto found=product.ports.find(s);if(found!=product.ports.end()) return found->second;
        found=product.tiedWires.find(s);if(found!=product.tiedWires.end()) return found->second;
        throw Unsupported("cannot size $past argument "+s);
    }
    std::string pastReg(const std::string &s,unsigned k) {
        auto key=std::make_pair(s,k); auto found=past.find(key);
        if(found!=past.end()) return found->second;
        if(k==0 || k>1024) throw Unsupported("history depth outside legacy encoding");
        auto w=width(s); std::string range=product.signedPorts.count(s)?"signed ":"";
        if(w!=1) range+="["+std::to_string(w-1)+":0] ";
        auto previous=s;std::string name;
        for(unsigned step=0;step<k;++step) {
            name="veval_past_"+std::to_string(past.size())+"_"+std::to_string(step);
            all.generated.insert(name);
            all.declarations.push_back("  reg "+range+name+"; initial "+name+" = "+std::to_string(w)+"'b0;");
            all.declarations.push_back("  always @(posedge "+clock+") "+name+" <= "+previous+";"); previous=name;
        }
        return past[key]=name;
    }
    std::string history(std::string s) {
        if(multiclock) {
            std::smatch stable;static const std::regex stableRe(R"(\$stable\s*\()");
            while(std::regex_search(s,stable,stableRe)) {
                const size_t open=stable.position()+stable.length()-1,close=balancedEnd(s,open);
                auto arg=trim(s.substr(open+1,close-open-1));auto old=pastReg(arg,1);
                s=s.substr(0,stable.position())+"("+arg+" == "+old+")"+s.substr(close+1);
            }
        }
        std::smatch m;static const std::regex re(R"(\$past\s*\()");
        while(std::regex_search(s,m,re)) {
            size_t open=m.position()+m.length()-1,close=balancedEnd(s,open);
            auto arg=s.substr(open+1,close-open-1);size_t comma=arg.npos;int depth=0;
            for(size_t i=0;i<arg.size();++i) {
                if(arg[i]=='(' || arg[i]=='{' || arg[i]=='[')++depth;
                else if(arg[i]==')' || arg[i]=='}' || arg[i]==']')--depth;
                else if(arg[i]==',' && !depth) {if(comma!=arg.npos) throw Unsupported("$past has more than two arguments");comma=i;}
            }
            unsigned k=1;
            if(comma!=arg.npos) {
                static const std::regex depthRe("[0-9]+");
                const auto value=trim(arg.substr(comma+1));
                if(!std::regex_match(value,depthRe))throw Unsupported("nonconstant $past depth");
                k=std::stoul(value);
            }
            auto target=trim(arg.substr(0,comma)),replacement=pastReg(target,k);
            s=s.substr(0,m.position())+replacement+s.substr(close+1);
        }
        return s;
    }
    std::vector<std::string> delayChain(const std::string &expr,unsigned k) {
        auto previous=expr;std::vector<std::string> names;
        for(unsigned i=0;i<k;++i) {
            auto name="veval_d"+std::to_string(++delays);all.generated.insert(name);names.push_back(name);
            all.declarations.push_back("  reg "+name+"; initial "+name+" = 1'b0;");
            all.declarations.push_back("  always @(posedge "+clock+") "+name+" <= "+previous+";");previous=name;
        }
        return names;
    }
    Encoding encode(std::string body) {
        if(samplingClock(product,body)!=clock)throw Unsupported("mixed property sampling clocks");
        auto rest=trim(body.substr(body.find(')')+1));std::string abort;bool hasAbort=false;std::smatch m;
        static const std::regex disable(R"(^disable\s+iff\s*\()"),
            nestedDisable(R"(\bdisable\b)"), leadingAlways(R"(^always\b(?!\s*\[))");
        if(std::regex_search(rest,m,disable)) {
            auto close=balancedEnd(rest,m.length()-1);hasAbort=true;
            abort=history(booleanTerm(rest.substr(m.length(),close-m.length())));rest=trim(rest.substr(close+1));
            if(std::regex_search(rest,nestedDisable)) throw Unsupported("nested disable iff");
        }
        rest=stripParens(rest);
        if(std::regex_search(rest,leadingAlways)) rest=trim(rest.substr(6));
        auto terms=flatten(rest);Encoding result;
        // The original weak-until shape is handled by the native typed compiler.
        std::vector<std::string> plain,nexted;std::set<unsigned> depths;
        for(const auto &t:terms) {
            auto value=multiclock?booleanTerm(history(t.body)):history(booleanTerm(t.body));
            if(t.next) {depths.insert(t.delay);nexted.push_back(value);}else plain.push_back(value);
        }
        if(depths.size()>1) throw Unsupported("disjuncts at differing depths");
        if(nexted.empty()) {
            if(hasAbort) plain.insert(plain.begin(),abort);
            // History registers start at zero; check the first sample too.
            result.expr=parenthesized(plain);result.guard="1'b1";
        } else {
            unsigned k=*depths.begin();auto next=parenthesized(nexted);result.expr=next;
            if(!plain.empty()) {
                if(!k) throw Unsupported("zero-delay window needs native compiler");
                auto held=delayChain("("+parenthesized(plain)+")",k).back();
                result.expr="("+held+") || ("+next+")";
            }
            if(hasAbort) {
                std::vector<std::string> parts{abort};auto delays=delayChain("("+abort+")",k);
                parts.insert(parts.end(),delays.begin(),delays.end());parts.push_back(result.expr);
                if(multiclock && k) {
                    // Latch asynchronous abort pulses between sampled clock edges.
                    auto name="veval_abort_"+std::to_string(++this->delays),event=name+"_event";
                    all.generated.insert(name);all.generated.insert(event);
                    all.declarations.push_back("  wire "+event+" = "+abort+";");
                    all.declarations.push_back("  reg "+name+"; initial "+name+" = 1'b0;");
                    all.declarations.push_back("  always @(posedge "+clock+" or posedge "+event+") if ("+event+") "+name+" <= 1'b1; else "+name+" <= 1'b0;");
                    parts.push_back(name);
                    // Only the k intervals following the starting sample may cancel it.
                    if(k>1){auto intervals=delayChain(name,k-1);parts.insert(parts.end(),intervals.begin(),intervals.end());}
                }
                result.expr=parenthesized(parts);
            }
            // A delay-k window starts being checked at sample k.
            result.guard=k==0?"1'b1":k==1?"veval_started":delayChain("1'b1",k).back();result.delay=k;
        }
        return result;
    }
    void checkNames(const Encoding &code, std::set<std::string> names) const {
        names.insert(all.generated.begin(), all.generated.end());
        declared(code.expr, names);
    }
    Encoding encodeBlock(const J &block, const std::set<std::string> &names,
                         const NativeEncoder &native) {
        const auto body = text(block["body"]);
        const auto saved = all;
        const auto savedPast = past;
        const auto savedDelays = delays;
        try {
            if (truth(block["local_declarations"]))
                throw Unsupported("local variables require the native data-register monitor");
            auto code = encode(body);
            // A misparsed keyword must trigger native fallback, not drop the assertion.
            checkNames(code, names);
            return code;
        } catch (const Unsupported &error) {
            if (!native) throw;
            static const std::regex disable(R"(\bdisable\s+iff\b)");
            if (multiclock && std::regex_search(body, disable))
                throw Unsupported("multiclock asynchronous disable iff requires the checked point-window encoding: " +
                                  std::string(error.what()));
            all = saved;
            past = savedPast;
            delays = savedDelays;
        }
        auto code = native(text(block["label"]), body, all.generated.size());
        all.declarations.insert(all.declarations.end(), code.declarations.begin(), code.declarations.end());
        all.generated.insert(code.generated.begin(), code.generated.end());
        checkNames(code, names);
        return code;
    }
};
static std::string selectedClock(const SbyProduct &p, const std::set<std::string> &targets, bool multiclock) {
    std::string clock;
    for(const auto &block:items(p.blocks)) {
        if(text(block["kind"])=="assert"&&!targets.count(text(block["label"])))continue;
        const auto sampled=samplingClock(p,text(block["body"]));
        if(!clock.empty()&&clock!=sampled)throw Unsupported("mixed property sampling clocks are not supported");
        clock=sampled;
    }
    if(clock.empty())throw Unsupported("no sampled property selected");
    // Clock-event models require the original event's whole-port binding.
    if(multiclock) {
        if(p.sampling.is_null())throw Unsupported("multiclock needs a clock-event product (veval transport --multiclock)");
        if(text(p.sampling["schema"])!="veval-sampling/1"||text(p.sampling["basis"])!="whole-binding"||text(p.sampling["port"])!=clock)
            throw Unsupported("multiclock: sampling port `"+clock+"' is not connected to the source clock `"+text(p.sampling["source"])+
                              "' by whole bindings ("+text(p.sampling["basis"])+")");
    }
    return clock;
}
static J resetContract(const SbyProduct &p) {
    bool needsReset=false;std::string resetPredicate;
    static const std::regex predicate(R"(\(predicate: (.*)\)$)"),
        name(R"((^|[^\w'$])([A-Za-z_]\w*))"), simplePredicate(R"(!?[A-Za-z_]\w*)");
    for(const auto &s:items(p.premises)) if(text(s).find("reset asserted at the initial instant")!=std::string::npos) {
        needsReset=true;std::smatch m;auto line=text(s);
        if(std::regex_search(line,m,predicate))resetPredicate=m[1];
    }
    if(!needsReset) return J();
    if(!resetPredicate.empty()) {
        for(std::sregex_iterator it(resetPredicate.begin(),resetPredicate.end(),name),end;it!=end;++it)
            if(!p.ports.count((*it)[2].str()))throw Unsupported("reset predicate names a non-port: "+resetPredicate);
        const bool simple=std::regex_match(resetPredicate,simplePredicate);
        auto assumption="veval_started || "+(simple?resetPredicate:"("+resetPredicate+")");
        return object({{"predicate",str(resetPredicate)},{"assume",str(assumption)}});
    }

    // Legacy products without a predicate require exactly one reset-like port.
    std::string resetPort;
    unsigned candidates=0;
    for(const auto &name:{"rstn","rst_n","resetn","reset_n","rst","reset"}) {
        const auto port=p.ports.find(name);
        if(port==p.ports.end()||port->second!=1)continue;
        resetPort=name;
        ++candidates;
    }
    if(candidates>1)throw Unsupported("reset premise without a predicate and more than one reset-like port");
    if(!candidates)throw Unsupported("reset premise stated but no reset port found");
    const bool low=resetPort!="rst"&&resetPort!="reset";
    const auto assumption="veval_started || "+std::string(low?"!":"")+resetPort;
    return object({{"port",str(resetPort)},{"polarity",str(low?"active-low":"active-high")},{"assume",str(assumption)}});
}
J buildWrapper(const SbyProduct &p,const std::set<std::string> &targets,const NativeEncoder &native,bool multiclock) {
    const auto clock=selectedClock(p,targets,multiclock);
    Encoder enc{p,multiclock,clock};
    std::vector<std::string> statements;
    const auto reset=resetContract(p);
    const bool needsReset=!reset.is_null();
    J derived=list(),kept=list(),dropped=list(),live=list();
    std::vector<std::pair<std::string,std::string>> witnesses;
    if(needsReset) witnesses.emplace_back("reset_contract_power_on",text(reset["assume"]));

    static const std::regex premiseRe(
        R"(^(\w+)(\[(\d+):(\d+)\])?\s*(==|<=|>=|<|>)\s*(\S+)\s*\(the (?:design's own tie-off|parent drives it through a clamp)\)$)");
    size_t index=0;
    for(const auto &premise:items(p.premises)) {
        auto line=text(premise),label="derived_premise_"+std::to_string(index++);std::smatch m;
        if(line.find("reset asserted at the initial instant")!=line.npos)continue;
        if(!std::regex_match(line,m,premiseRe)||!p.ports.count(m[1]))
            throw Unsupported("derived premise is outside checked shape: "+line);
        if(m[2].matched && (std::stoull(m[3]) < std::stoull(m[4]) ||
            std::stoull(m[3]) >= p.ports.at(m[1])))
            throw Unsupported("derived premise has an invalid port range: "+line);
        auto expr=m[1].str()+m[2].str()+" "+m[5].str()+" "+m[6].str();
        append(derived,object({{"label",str(label)},{"term",str(expr)}}));witnesses.emplace_back(label,expr);
        statements.push_back("  // "+label+" (derived premise)\n  always @* assume ("+expr+");");
    }
    std::set<std::string> names{"veval_started"};for(const auto &port:p.ports)names.insert(port.first);
    for(const auto &wire:p.tiedWires)names.insert(wire.first);
    std::set<std::string> sampledAssumptions;
    for(const auto &block:items(p.blocks)) {
        auto label=text(block["label"]),kind=text(block["kind"]);
        if(kind=="assert"&&!targets.count(label))continue;
        Encoding code;
        try {
            code=enc.encodeBlock(block,names,native);
            if(code.mode=="live") {if(kind=="assume")throw Unsupported("unbounded environmental fairness requires a fairness-aware reduction");append(live,str(label));}
        } catch(const Unsupported &e) {
            if(kind=="assume")throw Unsupported("assume "+label+" not encodable: "+e.what());
            append(dropped,object({{"label",str(label)},{"reason",str(e.what())}}));continue;
        }
        if(multiclock && code.mode=="live")throw Unsupported("multiclock liveness reduction is not implemented");
        // Clock-event models check only the sampled edge, not both phases.
        const std::string event=multiclock?"@(posedge "+clock+")":"@*";
        statements.push_back("  // "+label+" ("+kind+")\n  always "+event+" if ("+code.guard+") "+kind+" ("+(code.mode=="live"?"s_eventually ":"")+code.expr+");");
        append(kept,object({{"label",str(label)},{"kind",str(kind)},{"delay",num(code.delay)}}));
        if(kind=="assume") {
            witnesses.emplace_back(label,"!("+code.guard+") || ("+code.expr+")");
            sampledAssumptions.insert(label);
        }
    }
    std::vector<std::string> lines{p.prelude,""};
    while(!lines[0].empty() && std::isspace(static_cast<unsigned char>(lines[0].back())))lines[0].pop_back();
    bool started=needsReset;
    for(const auto &s:statements)if(s.find("veval_started")!=s.npos)started=true;
    if(started) {lines.push_back("  // sampling starts after the first clock edge");lines.push_back("  reg veval_started; initial veval_started = 1'b0;");lines.push_back("  always @(posedge "+clock+") veval_started <= 1'b1;");}
    if(needsReset){lines.push_back("  // reset contract, carried by the product as a `premise:' line only");lines.push_back("  always @* assume ("+text(reset["assume"])+");");}
    if(started)lines.push_back("");
    lines.insert(lines.end(),enc.all.declarations.begin(),enc.all.declarations.end());lines.push_back("");
    lines.insert(lines.end(),statements.begin(),statements.end());lines.push_back("");
    lines.push_back("  // premise witnesses: each must be 1 in every timeframe of any");
    lines.push_back("  // counterexample, or the engine answered a weaker question.");
    J witnessRows=list(),asserts=list(),assumes=list(),generated=list();
    for(size_t i=0;i<witnesses.size();++i){
        auto name="veval_prem_"+std::to_string(i);
        const bool sampled=multiclock && sampledAssumptions.count(witnesses[i].first);
        if(sampled) {
            // Initial 1 denotes no sample yet. The registered witness records
            // precisely the value checked by the edge-triggered assumption.
            lines.push_back("  (* keep *) reg "+name+" = 1'b1;  // "+witnesses[i].first);
            lines.push_back("  always @(posedge "+clock+") "+name+" <= "+witnesses[i].second+";");
        } else lines.push_back("  (* keep *) wire "+name+" = "+witnesses[i].second+";  // "+witnesses[i].first);
        enc.all.generated.insert(name);append(witnessRows,object({{"index",num(i)},{"label",str(witnesses[i].first)}}));
    }
    if(started)enc.all.generated.insert("veval_started");
    for(const auto &name:enc.all.generated)append(generated,str(name));
    for(const auto &k:items(kept))append(text(k["kind"])=="assert"?asserts:assumes,k["label"]);
    lines.push_back("endmodule");
    // Empty lines are significant in the established wrapper output.
    std::ostringstream rendered;for(const auto &line:lines)rendered<<line<<'\n';
    return object({{"text",str(rendered.str())},{"generated",generated},{"asserts",asserts},{"assumes",assumes},
                   {"dropped_asserts",dropped},{"derived_premises",derived},{"premise_witnesses",witnessRows},{"reset",reset},{"liveness",live}});
}
} // namespace veval::verification
