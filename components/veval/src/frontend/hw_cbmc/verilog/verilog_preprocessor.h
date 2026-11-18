#ifndef VERILOG_PREPROCESSOR_H
#define VERILOG_PREPROCESSOR_H

#include <util/invariant.h>
#include <util/irep.h>
#include <util/preprocessor.h>
#include <util/string_hash.h>

#include "verilog_preprocessor_tokenizer.h"

#include <filesystem>
#include <list>
#include <map>

class verilog_preprocessort:public preprocessort
{
public:
  virtual void preprocessor();

  verilog_preprocessort(
    std::istream &_in,
    std::ostream &_out,
    message_handlert &_message_handler,
    const std::string &_filename,
    const std::list<std::string> &_include_paths,
    const std::list<std::string> &_initial_defines)
    : preprocessort(_in, _out, _message_handler, _filename),
      include_paths(_include_paths),
      initial_defines(_initial_defines)
  {
    condition=true;
  }

  virtual ~verilog_preprocessort() { }

protected:
  // from the command line
  const std::list<std::string> &include_paths;
  const std::list<std::string> &initial_defines;

  using tokent = verilog_preprocessor_token_sourcet::tokent;

  struct definet
  {
    /// A parameter, with the default value it takes when the invocation
    /// leaves it out (1800-2017 22.5.1, SystemVerilog 2009 onwards).
    struct parametert
    {
      std::string name;
      bool has_default = false;
      std::vector<tokent> default_value;

      parametert() = default;
      explicit parametert(std::string _name) : name(std::move(_name))
      {
      }
    };

    using parameterst = std::vector<parametert>;
    parameterst parameters;
    std::vector<tokent> tokens;
    definet() = default;
    explicit definet(std::vector<tokent> _tokens) : tokens(std::move(_tokens))
    {
    }
  };

  static std::string as_string(const std::vector<tokent> &);

  using definest = std::unordered_map<std::string, definet, string_hash>;
  definest defines;

  /// Text macros the source text of this compilation unit has defined so far.
  ///
  /// 1800-2017 22.5.1: a `define is in effect from where it appears to the
  /// end of the compilation unit, which by default is every file named on
  /// one command line. A preprocessor is constructed per file, so without
  /// carrying the table across them an include guard cannot do its job: a
  /// header included from two files is expanded twice, and whatever it
  /// declares -- typically a package -- is then declared twice.
  ///
  /// Only `define, `undef and `undefineall in the source text change this
  /// table. The initial defines a preprocessor is constructed with belong to
  /// that file's command line: they are applied to the file (a persisted
  /// source-text macro of the same name wins) and are not handed on, so a
  /// caller that preprocesses files under different command-line options,
  /// as a tool script does, sees the same macro state Yosys's read_verilog
  /// would: source-text macros persist across files and commands,
  /// command-line macros do not.
  static definest compilation_unit_defines;

public:
  /// Begin a fresh compilation unit, discarding the macros of the last one.
  ///
  /// One process elaborates one compilation unit in the usual flow, so this
  /// is never needed there. A tool that elaborates several designs in a row
  /// -- a parent and then each of its children, say -- must call this
  /// between them, or the second design finds the first one's include guards
  /// already set, skips the headers behind them, and fails on the
  /// declarations it never saw.
  static void new_compilation_unit()
  {
    compilation_unit_defines.clear();
  }

protected:

  void directive();
  std::filesystem::path find_include_file(
    const std::filesystem::path &including_file,
    const std::string &given_filename,
    bool include_paths_only);
  definet::parameterst parse_define_parameters();

  using define_argumentst = std::map<std::string, std::vector<tokent>>;
  define_argumentst parse_define_arguments(const definet &);

  // for ifdef, else, endif
  
  bool condition;
  
  class conditionalt
  {
  public:
    bool condition;
    bool previous_condition;
    bool else_part;

    /// Whether any branch of this `ifdef chain has already been taken.
    /// Without it an `elsif chain is not exclusive: `condition' alone says
    /// only whether the CURRENT branch matches, so after
    ///   `ifdef A ... `elsif B ... `else ... `endif
    /// with A defined and B not, the `else part is taken as well, and both
    /// bodies are compiled.
    bool any_taken;

    conditionalt() : else_part(false), any_taken(false)
    {
    }

    bool get_cond()
    {
      if(!previous_condition)
        return false;

      return else_part ? !any_taken : condition;
    }
  };

  std::list<conditionalt> conditionals;

  class vector_token_sourcet : public verilog_preprocessor_token_sourcet
  {
  public:
    vector_token_sourcet(const std::vector<tokent> &_tokens)
      : tokens(_tokens), pos(tokens.begin())
    {
    }

  protected:
    const std::vector<tokent> &tokens;
    std::vector<tokent>::const_iterator pos;
    void get_token_from_stream() override;
  };

  // To synchronize the parser's line number
  std::size_t parser_line_no = 0;
  void emit_line_directive(unsigned level);

  // for include and for `define
  class contextt
  {
  protected:
    bool deallocate_in;
    std::istream *in;

  public:
    std::filesystem::path path;
    verilog_preprocessor_token_sourcet *tokenizer;

    std::string filename_as_string() const;

    // for `define with parameters
    define_argumentst define_arguments;

    contextt(
      bool _deallocate_in,
      std::istream *_in,
      std::filesystem::path _path)
      : deallocate_in(_deallocate_in),
        in(_in),
        path(std::move(_path)),
        tokenizer(new verilog_preprocessor_tokenizert(*in))
    {
    }

    explicit contextt(const std::vector<tokent> &tokens)
      : deallocate_in(false),
        in(nullptr),
        path(),
        tokenizer(new vector_token_sourcet(tokens))
    {
    }

    ~contextt()
    {
      delete tokenizer;
      if(deallocate_in)
        delete in;
    }

    source_locationt make_source_location() const;

    bool is_file() const
    {
      return !path.empty();
    }
  };

  std::list<contextt> context_stack;

  // the topmost context
  contextt &context()
  {
    PRECONDITION(!context_stack.empty());
    return context_stack.back();
  }

  // the topmost tokenizer
  verilog_preprocessor_token_sourcet &tokenizer()
  {
    return *(context().tokenizer);
  }
};

#endif
