// Runtime C injecté en tête du fichier instrumenté.
// Toutes les fonctions renvoient un int pour pouvoir être chaînées
// dans des expressions à virgule (conditions, return, for).
static const char *CTraceRuntime = R"CTRT(/* ---- ctrace runtime ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__attribute__((weak)) int __ct_depth;
__attribute__((weak)) int __ct_cv;
__attribute__((weak)) FILE *__ct_fp;
/* état de la ligne en cours : 0 normal, 1 avant le premier « // », 2 avant le
   premier paramètre, 3 paramètres suivants */
__attribute__((weak)) int __ct_st;
__attribute__((weak)) int __ct_col; /* longueur du code de la ligne en cours */
__attribute__((weak)) int __ct_lw;  /* largeur de la colonne « fichier:ligne » */
#define __CT_COMMENT_COL 32
static __attribute__((unused)) FILE *__ct_out(void) {
  if (!__ct_fp) {
    const char *p = getenv("CTRACE_OUT");
    if (p && *p) __ct_fp = fopen(p, "w");
    if (!__ct_fp) __ct_fp = stderr;
    else setvbuf(__ct_fp, NULL, _IOLBF, 0);
  }
  return __ct_fp;
}
static __attribute__((unused)) void __ct_indent(void) {
  int i; FILE *f = __ct_out();
  for (i = 1; i < __ct_depth; i++) fputs("  ", f);
}
static __attribute__((unused)) void __ct_name(const char *n) {
  FILE *f = __ct_out();
  if (__ct_st == 1) {
    int i = __ct_col;
    do fputc(' ', f); while (++i < __CT_COMMENT_COL);
    fputs("//", f);
  } else if (__ct_st == 2) {
    __ct_st = 3; fprintf(f, "%s=", n); return;
  } else if (__ct_st == 3) {
    fprintf(f, ", %s=", n); return;
  }
  __ct_st = 0;
  if (*n) fprintf(f, " %s=", n); else fputs(" = ", f);
}
/* struct retournée : " = {x=1, y=2}" (champs séparés comme les paramètres) */
static __attribute__((unused)) int __ct_rec(void) {
  __ct_name(""); fputc('{', __ct_out()); __ct_st = 2; return 0;
}
static __attribute__((unused)) int __ct_rec_end(void) {
  __ct_st = 0; fputc('}', __ct_out()); return 0;
}
static __attribute__((unused)) void __ct_loc(const char *loc) {
  __ct_indent(); __ct_lw = (int)strlen(loc); fprintf(__ct_out(), "%s ", loc);
}
static __attribute__((unused)) void __ct_blank(void) {
  __ct_indent(); fprintf(__ct_out(), "%*s ", __ct_lw, "");
}
static __attribute__((unused)) int __ct_i(const char *n, long long v) {
  __ct_name(n); fprintf(__ct_out(), "%lld", v); return 0;
}
static __attribute__((unused)) int __ct_u(const char *n, unsigned long long v) {
  __ct_name(n); fprintf(__ct_out(), "%llu", v); return 0;
}
static __attribute__((unused)) int __ct_f(const char *n, long double v) {
  __ct_name(n); fprintf(__ct_out(), "%Lg", v); return 0;
}
static __attribute__((unused)) int __ct_b(const char *n, int v) {
  __ct_name(n); fputs(v ? "true" : "false", __ct_out()); return 0;
}
static __attribute__((unused)) int __ct_c(const char *n, int v) {
  __ct_name(n);
  if (v >= 32 && v < 127) fprintf(__ct_out(), "'%c'", v);
  else fprintf(__ct_out(), "%d", v);
  return 0;
}
static __attribute__((unused)) int __ct_p(const char *n, const void *v) {
  __ct_name(n);
  if (v) fprintf(__ct_out(), "%p", v); else fputs("NULL", __ct_out());
  return 0;
}
static __attribute__((unused)) int __ct_s(const char *n, const char *s, unsigned long cap) {
  unsigned long i; FILE *f = __ct_out();
  __ct_name(n); fputc('"', f);
  for (i = 0; i < cap && i < 64 && s[i]; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
    else if (c >= 32 && c < 127) fputc(c, f);
    else fprintf(f, "\\x%02x", c);
  }
  fputc('"', f);
  if (i == 64 && i < cap && s[i]) fputs("...", f);
  return 0;
}
/* kind: 0 signé, 1 non signé, 2 flottant, 3 pointeur */
static __attribute__((unused)) int __ct_arr(const char *n, const void *p, unsigned long count,
                                            unsigned long esz, int kind) {
  unsigned long i; FILE *f = __ct_out();
  const unsigned char *b = (const unsigned char *)p;
  __ct_name(n); fputc('{', f);
  for (i = 0; i < count && i < 8; i++) {
    const void *e = b + i * esz;
    if (i) fputs(", ", f);
    if (kind == 2) {
      if (esz == sizeof(float)) { float v; memcpy(&v, e, sizeof v); fprintf(f, "%g", v); }
      else if (esz == sizeof(double)) { double v; memcpy(&v, e, sizeof v); fprintf(f, "%g", v); }
      else { long double v; memcpy(&v, e, sizeof v); fprintf(f, "%Lg", v); }
    } else if (kind == 3) {
      void *v; memcpy(&v, e, sizeof v); fprintf(f, "%p", v);
    } else {
      unsigned long long u = 0; long long s = 0;
      switch (esz) {
      case 1: { unsigned char v; memcpy(&v, e, 1); u = v; s = (signed char)v; break; }
      case 2: { unsigned short v; memcpy(&v, e, 2); u = v; s = (short)v; break; }
      case 4: { unsigned int v; memcpy(&v, e, 4); u = v; s = (int)v; break; }
      default: { unsigned long long v; memcpy(&v, e, 8); u = v; s = (long long)v; break; }
      }
      if (kind == 0) fprintf(f, "%lld", s); else fprintf(f, "%llu", u);
    }
  }
  if (count > 8) fputs(", ...", f);
  fputc('}', f);
  return 0;
}
/* a.c:38   s = add(s, 2);              // s=0 */
static __attribute__((unused)) int __ct_pre(const char *loc, const char *code) {
  __ct_loc(loc); fprintf(__ct_out(), "  %s", code);
  __ct_col = 2 + (int)strlen(code); __ct_st = 1; return 0;
}
static __attribute__((unused)) int __ct_code(void) {
  __ct_st = 0; fputc('\n', __ct_out()); return 0;
}
/*            => s=2 */
static __attribute__((unused)) int __ct_post(void) {
  __ct_blank(); fputs("=>", __ct_out()); __ct_st = 0; return 0;
}
static __attribute__((unused)) int __ct_end(void) {
  fputc('\n', __ct_out()); return 0;
}
static __attribute__((unused)) int __ct_cond(int v) {
  __ct_blank(); fprintf(__ct_out(), "-> %s\n", v ? "true" : "false"); return v;
}
/* a.c:6    -> add(a=0, b=2) */
static __attribute__((unused)) int __ct_enter(const char *loc, const char *fn) {
  __ct_depth++; __ct_loc(loc); fprintf(__ct_out(), "-> %s(", fn); __ct_st = 2; return 0;
}
static __attribute__((unused)) int __ct_args(void) {
  __ct_st = 0; fputs(")\n", __ct_out()); return 0;
}
static __attribute__((unused)) int __ct_ret(const char *loc, const char *fn) {
  __ct_loc(loc); fprintf(__ct_out(), "<- %s", fn); __ct_st = 0; return 0;
}
static __attribute__((unused)) int __ct_leave(void) {
  fputc('\n', __ct_out()); __ct_depth--; return 0;
}
/* ---- fin runtime ---- */
)CTRT";
