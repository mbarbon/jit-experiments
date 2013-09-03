#ifndef PJ_TYPES_H_
#define PJ_TYPES_H_

#include <string>

// TODO should this have a pj_undef_type? If so, change the AST::UndefConstant class.
enum pj_type_id {
  pj_unspecified_type,
  pj_any_type,
  pj_sv_type,
  pj_gv_type,
  pj_opaque_type,
  pj_array_type,
  pj_hash_type,
  pj_string_type,
  pj_double_type,
  pj_int_type,
  pj_uint_type
};

namespace PerlJIT {
  namespace AST {
    class Type {
    public:
      virtual ~Type();
      virtual pj_type_id tag() const = 0;

      virtual bool equals(Type *other) const = 0;

      virtual bool is_scalar() const { return false; }
      virtual bool is_array() const { return false; }
      virtual bool is_hash() const { return false; }
      virtual bool is_opaque() const { return false; }
      virtual bool is_unspecified() const { return false; }

      virtual bool is_xv() const { return false; }
      virtual bool is_integer() const { return false; }
      virtual bool is_numeric() const { return false; }

      virtual std::string to_string() const = 0;
      virtual const char *perl_class() const
        { return "Perl::JIT::AST::Type"; }
    };

    class Scalar : public Type {
    public:
      Scalar(pj_type_id tag);
      virtual pj_type_id tag() const;

      virtual bool equals(Type *other) const;

      virtual bool is_scalar() const { return true; }
      virtual bool is_unspecified() const;
      virtual bool is_opaque() const;

      virtual bool is_xv() const;

      virtual bool is_integer() const;
      virtual bool is_numeric() const;

      virtual std::string to_string() const;
      virtual const char *perl_class() const
        { return "Perl::JIT::AST::Scalar"; }
    private:
      pj_type_id _tag;
    };

    class Array : public Type {
    public:
      Array(Type *element);
      virtual pj_type_id tag() const;
      Type *element() const;

      virtual bool equals(Type *other) const;

      virtual bool is_array() const { return true; }

      virtual std::string to_string() const;
      virtual const char *perl_class() const
        { return "Perl::JIT::AST::Array"; }
    private:
      Type *_element;
    };

    class Hash : public Type {
    public:
      Hash(Type *element);
      virtual pj_type_id tag() const;
      Type *element() const;

      virtual bool equals(Type *other) const;

      virtual bool is_hash() const { return true; }

      virtual std::string to_string() const;
      virtual const char *perl_class() const
        { return "Perl::JIT::AST::Hash"; }
    private:
      Type *_element;
    };

    Type *parse_type(const std::string &str);
  }
}

#endif // PJ_TYPES_H_
