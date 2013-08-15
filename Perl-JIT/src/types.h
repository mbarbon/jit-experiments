#ifndef PJ_TYPES_H_
#define PJ_TYPES_H_

enum pj_type_id {
  pj_unspecified_type,
  pj_scalar_type,
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
    };

    class Scalar : public Type {
    public:
      Scalar(pj_type_id tag);
      virtual pj_type_id tag() const;

    private:
      pj_type_id _tag;
    };

    Type *parse_type(const char *string);
  }
}

#endif // PJ_TYPES_H_
