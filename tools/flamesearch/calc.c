#include "calc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// a sum read by recursive descent:
//   sum     := product (('+' | '-') product)*
//   product := power (('*' | '/' | '%') power)*
//   power   := unary ('^' power)?
//   unary   := '-' unary | '+' unary | atom
//   atom    := number | name | name '(' sum ')' | '(' sum ')'

struct reader {
	const char *at;
	bool failed;
	int ops; // working done: operators, functions
};

static double sum(struct reader *r);

static void space(struct reader *r) {
	while (isspace((unsigned char)*r->at)) {
		r->at++;
	}
}

// the ones typed as they look: × ÷ − (utf-8), read as * / -
static char op(struct reader *r) {
	space(r);
	static const struct {
		const char *text;
		char op;
	} wide[] = {{"\xc3\x97", '*'}, {"\xc3\xb7", '/'}, {"\xe2\x88\x92", '-'}};
	for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); i++) {
		if (!strncmp(r->at, wide[i].text, strlen(wide[i].text))) {
			return wide[i].op;
		}
	}
	return *r->at;
}

static void take(struct reader *r) {
	unsigned char c = (unsigned char)*r->at;
	r->at += c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
}

static double atom(struct reader *r) {
	space(r);
	if (*r->at == '(') {
		r->at++;
		double v = sum(r);
		space(r);
		if (*r->at != ')') {
			r->failed = true;
		} else {
			r->at++;
		}
		return v;
	}
	if (isdigit((unsigned char)*r->at) || *r->at == '.') {
		char *end;
		double v = strtod(r->at, &end);
		if (end == r->at) {
			r->failed = true;
		}
		r->at = end;
		return v;
	}
	if (isalpha((unsigned char)*r->at)) {
		char name[16];
		size_t len = 0;
		while (isalpha((unsigned char)*r->at) && len < sizeof(name) - 1) {
			name[len++] = (char)tolower((unsigned char)*r->at++);
		}
		name[len] = '\0';
		if (!strcmp(name, "pi")) {
			return M_PI;
		}
		if (!strcmp(name, "e")) {
			return M_E;
		}
		static const struct {
			const char *name;
			double (*f)(double);
		} functions[] = {
			{"sqrt", sqrt}, {"sin", sin}, {"cos", cos}, {"tan", tan}, {"ln", log},
			{"log", log10}, {"abs", fabs}, {"round", round}, {"floor", floor}, {"ceil", ceil},
		};
		for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
			if (!strcmp(name, functions[i].name)) {
				space(r);
				if (*r->at != '(') {
					break;
				}
				r->ops++;
				return functions[i].f(atom(r));
			}
		}
	}
	r->failed = true;
	return 0;
}

static double unary(struct reader *r) {
	char c = op(r);
	if (c == '-' || c == '+') {
		take(r);
		double v = unary(r);
		return c == '-' ? -v : v;
	}
	return atom(r);
}

static double power(struct reader *r) {
	double v = unary(r);
	if (op(r) == '^') {
		take(r);
		r->ops++;
		return pow(v, power(r)); // 2^3^2 is 2^9
	}
	return v;
}

static double product(struct reader *r) {
	double v = power(r);
	for (;;) {
		char c = op(r);
		if (c != '*' && c != '/' && c != '%') {
			return v;
		}
		take(r);
		r->ops++;
		double w = power(r);
		v = c == '*' ? v * w : c == '/' ? v / w : fmod(v, w);
	}
}

static double sum(struct reader *r) {
	double v = product(r);
	for (;;) {
		char c = op(r);
		if (c != '+' && c != '-') {
			return v;
		}
		take(r);
		r->ops++;
		double w = product(r);
		v = c == '+' ? v + w : v - w;
	}
}

bool calc(const char *text, char *out, size_t size) {
	struct reader r = {.at = text};
	double v = sum(&r);
	space(&r);
	if (r.failed || *r.at || !r.ops || !isfinite(v)) {
		return false;
	}
	// as few digits as it needs, no 0.30000000000000004
	char number[64];
	snprintf(number, sizeof(number), "%.12g", v);
	if (!strcmp(number, "-0")) {
		snprintf(number, sizeof(number), "0");
	}
	snprintf(out, size, "%s", number);
	return true;
}
