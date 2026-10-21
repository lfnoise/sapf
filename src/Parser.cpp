//    SAPF - Sound As Pure Form
//    Copyright (C) 2019 James McCartney
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Parser.hpp"
#include "Opcode.hpp"
#include <ctype.h>
#include <cmath>
#include <vector>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma mark PARSER

bool parseElem(Thread& th, P<Code>& code);
bool parseWord(Thread& th, P<Code>& code);
static void bindVar(Thread& th, P<String> const& name, P<Code>& code);

class ParsingWhat
{
	Thread& th;
	int what;
public:	
	ParsingWhat(Thread& inThread, int newMode) : th(inThread), what(th.parsingWhat)
	{
		th.parsingWhat = newMode;
	} 
	~ParsingWhat()
	{
		th.parsingWhat = what;
	}
};


static bool skipSpace(Thread& th)
{
	//ScopeLog sl("skipSpace");
	for (;;) {
		int c = th.getc();
		if (c == ';') {
			c = th.getc();
			while (c && c != '\n') { c = th.getc(); }
			if (c == 0) { th.unget(1); return true; }
		}
		if (c == 0) { th.unget(1); return true; }
		bool skip = isspace(c) || iscntrl(c);
		if (!skip) { th.unget(1); break; }
	}
	return false;
}

const char* nonnamechars = ";()[]{}.`,:\"\n=";

static bool endOfWord(int c)
{
	return c == 0 || isspace(c) || strchr(nonnamechars, c) != nullptr;
}

static bool parseHexNumber(Thread& th, P<Code>& code)
{
	const char* start = th.curline();
	int64_t z = 0;
	
	th.getc();
	th.getc();
	int c = th.getc();
	while(isxdigit(c)) {
		if (isdigit(c)) z = z*16 + c - '0';
		else z = z*16 + toupper(c) - 'A' + 10;
		c = th.getc();
	}

	if (!endOfWord(c)) {
		// even though it starts out like a number it continues as some other token
		th.unget(start);
		return false;
	} else {
		th.unget(1);
	}
	
	code->add(opPushImmediate, z);
	
	return true;
}


static bool parseFloat(Thread& th, Z& result)
{	
	//ScopeLog sl("parseFloat");
	const char* start = th.curline();
	int c = th.getc();
    
    if (c == 'p' && th.c() == 'i') {
        th.getc();
        result = M_PI;
        return true;
    }
    
	if (c == '+' || c == '-') 
		c = th.getc();

	int digits = 0;
	bool sawdot = false;
	for ( ; ; ) {
		if (isdigit(c)) digits++;
		else if (c == '.') {
			if (sawdot) break;
			sawdot = true; 
		}
		else break;
		c = th.getc(); 
	}
	if (digits == 0) {
		th.unget(start);
		return false;
	}
	
	if (c == 'e' || c == 'E') {
		c = th.getc();
		if (c == '+' || c == '-') 
			c = th.getc();
		while (isdigit(c)) { c = th.getc(); }
	}

	th.toToken(start, (int)(th.curline() - start));
	
	bool sawpi = false;
	bool sawmega = false;
	bool sawkilo = false;
	bool sawhecto = false;
	bool sawcenti = false;
	bool sawmilli = false;
	bool sawmicro = false;
	
	
	if (c == 'p' && th.c() == 'i') {
		sawpi = true;
		th.getc();
	} else if (c == 'M') {
		sawmega = true;
	} else if (c == 'k') {
		sawkilo = true;
	} else if (c == 'h') {
		sawhecto = true;
	} else if (c == 'c') {
		sawcenti = true;
	} else if (c == 'm') {
		sawmilli = true;
	} else if (c == 'u') {
		sawmicro = true;
	} else {
		th.unget(1);
	}

	double x = strtod(th.token, nullptr);
	if (sawpi) x *= M_PI;
	else if (sawmega) x *= 1e6;
	else if (sawkilo) x *= 1e3;
	else if (sawhecto) x *= 1e2;
	else if (sawcenti) x *= 1e-2;
	else if (sawmilli) x *= 1e-3;
	else if (sawmicro) x *= 1e-6;

	result = x;
	return true;
}

static bool parseNumber(Thread& th, Z& result)
{
	const char* start = th.curline();

	Z a, b;
	if (parseFloat(th, a)) {
		if (th.c() == '/') {
			th.getc();
			if (parseFloat(th, b) && endOfWord(th.c())) {
				result = a/b;
				return true;
			}
		} else if (endOfWord(th.c())) {
			result = a;
			return true;
		}
	}
	th.unget(start);
	return false;
}

static bool parseNumber(Thread& th, P<Code>& code)
{
    Z x;
    if (parseNumber(th, x)) {
 		code->add(opPushImmediate, x);
        return true;
    }
    return false;
}

static bool parseSymbol(Thread& th, P<String>& result);
static bool parseItemList(Thread& th, P<Code>& code, int endbrace);


static bool parseQuote(Thread& th, P<Code>& code)
{
	th.getc();
	P<String> name;

	if (!parseSymbol(th, name)) 
		syntaxError("expected symbol after quote");
	
	V vname(name);
	code->add(opPushImmediate, vname);

	return true; 
}

static bool parseBackquote(Thread& th, P<Code>& code)
{
	th.getc();
	P<String> name;

	if (!parseSymbol(th, name)) 
		syntaxError("expected symbol after backquote");


	V vname(name);
	V val;
	size_t index;
	int scope = th.mCompileScope->indirectLookup(th, name, index, val);
	switch (scope) {
		case scopeLocal :
			val.i = index;
			code->add(opPushLocalVar, val);
			break;
		case scopeFunVar :
			val.i = index;
			code->add(opPushFunVar, val);
			break;
		case scopeBuiltIn :
			code->add(opPushImmediate, val);
			break;
		case scopeWorkspace :
			code->add(opPushWorkspaceVar, vname);
			break;
		default :
			post("backquote error: \"%s\" is an undefined word\n", name->s);
			syntaxError("undefined word");
	}

	return true; 
}

static bool parseDot(Thread& th, P<Code>& code)
{
	th.getc();
	P<String> name;

	if (!parseSymbol(th, name)) 
		syntaxError("expected symbol after dot");
	
	V vname(name);
	code->add(opDot, vname);

	return true; 
}

static bool parseComma(Thread& th, P<Code>& code)
{
	th.getc();
	P<String> name;

	if (!parseSymbol(th, name)) 
		syntaxError("expected symbol after dot");
	
	V vname(name);
	code->add(opComma, vname);

	return true; 
}

static bool parseColon(Thread& th, P<Code>& code)
{
	th.getc();

    P<String> name;

    if (!parseSymbol(th, name)) 
        syntaxError("expected symbol after colon");
    
    code->keys.push_back(name);

	return true;
}

static bool parseEachOp(Thread& th, P<Code>& code)
{
	
	int64_t mask = 0;
	int level = 0;

	th.getc();
	int c = th.getc();
	
	if (c == '@') {
		mask = 1; 
		++level;
		do {
			mask |= 1LL << level;
			++level;
			c = th.getc();
		} while (c == '@');
	} else if (c >= '2' && c <= '9') {
		mask = 1LL << (c - '1');
		c = th.getc();
	} else if (c == '0' || c == '1') {
		do { 
			if (c == '1')
				mask |= 1LL << level;
			++level;
			c = th.getc();
		} while (c == '0' || c == '1');
	} else {
		mask = 1;
	}
	if (isdigit(c)) {
		syntaxError("unexpected extra digit after @");
	}
	
	th.unget(1);

	V v;
	v.i = mask;
	
	code->add(opEach, v);
	
	
	return true; 
}


static bool parseNewForm(Thread& th, P<Code>& code)
{
	ParsingWhat pw(th, parsingEnvir);
	P<Code> code2 = new Code(8);
	th.getc();
	
	parseItemList(th, code2, '}');
	
	if (code2->keys.size()) {

		P<TableMap> tmap = new TableMap(code2->keys.size());
		
		int i = 0;
		for (Arg key : code2->keys) {
            tmap->put(i, key, key.Hash());
			++i;
		}

		code2->keys.clear();
		code2->add(opPushImmediate, V(tmap));
		code2->add(opReturn, 0.);
		code2->shrinkToFit();

		code->add(opNewForm, V(code2));
	} else {
		code2->add(opReturn, 0.);
		code2->shrinkToFit();
		code->add(opInherit, V(code2));
	}
			
	return true;
}

static String* parseString(Thread& th);

static bool parseStackEffect(Thread& th, int& outTakes, int& outLeaves)
{
	outTakes = 0;
	outLeaves = 1;
	skipSpace(th);
	int c = th.c();
	if (!isdigit(c)) return true;
	outLeaves = 0;
	
	while(isdigit(c)) {
		outTakes = outTakes * 10 + c - '0';
		c = th.getc();
	}
	if (c != '.') return false;
	c = th.getc();
	while(isdigit(c)) {
		outLeaves = outLeaves * 10 + c - '0';
		c = th.getc();
	}
	return true;
}

static bool parseLambda(Thread& th, P<Code>& code)
{
	//ScopeLog sl("parseLambda");
	ParsingWhat pw(th, parsingLambda);
	th.getc();
	std::vector<P<String> > args;
		
	SaveCompileScope scs(th);
	
	P<InnerCompileScope> cs = new InnerCompileScope(th.mCompileScope);
	th.mCompileScope = cs();
	
	while (1) {
		P<String> name;
		if (!parseSymbol(th, name)) break;
		args.push_back(name);
		
		int takes, leaves;
		if (!parseStackEffect(th, takes, leaves)) {
			syntaxError("incorrectly formatted function argument stack effect annotation.");
		}
		
		LocalDef def;
		def.mName = name;
		def.mIndex = cs->mLocals.size();
		def.mTakes = takes;
		def.mLeaves = leaves;
		cs->mLocals.push_back(def);
	}
	
	skipSpace(th);
	
	P<String> help = parseString(th);

	skipSpace(th);
	
	int c = th.getc();	
	if (c != '[') {
        post("got char '%c' %d\n", c, c);
		syntaxError("expected open square bracket after argument list");
	}
		
	P<Code> code2 = new Code(8);	
	parseItemList(th, code2, ']');

	code2->add(opReturn, 0.);
	code2->shrinkToFit();
	
		
	// compile code to push all fun vars
	for (size_t i = 0; i < cs->mVars.size(); ++i) {
		VarDef& def = cs->mVars[i];
		V vindex;
		vindex.i = def.mFromIndex;
		
		if (def.mFromScope == scopeLocal) {
			code->add(opPushLocalVar, vindex);
		} else {
			code->add(opPushFunVar, vindex);
		}
	}
	if (args.size() > USHRT_MAX || cs->mLocals.size() > USHRT_MAX || cs->mVars.size() > USHRT_MAX)
	{
		post("Too many variables!\n");
		throw errSyntax;
	}

    FunDef* def = new FunDef(th, code2, args.size(), cs->mLocals.size(), cs->mVars.size(), help);
	def->mArgNames = args;
	code->add(opPushFun, def);

	return true;
}

#define COMPILE_PARENS 1

static bool parseParens(Thread& th, P<Code>& code)
{
	ParsingWhat pw(th, parsingParens);
	th.getc();

#if COMPILE_PARENS
	P<Code> code2 = new Code(8);
	parseItemList(th, code2, ')');

	code2->add(opReturn, 0.);
	code2->shrinkToFit();
	code->add(opParens, V(code2));
#else
	parseItemList(th, code, ')');
#endif
			
	return true;
}

static bool parseArray(Thread& th, P<Code>& code)
{
	//ScopeLog sl("parseArray");
	ParsingWhat pw(th, parsingArray);
	th.getc();
	P<Code> code2 = new Code(8);
	parseItemList(th, code2, ']');

	if (code2->size()) {
		code2->add(opReturn, 0.);
		code2->shrinkToFit();
		code->add(opNewVList, V(code2));
	} else {
		code->add(opPushImmediate, V(vm._nilv));
	}

			
	return true;
}

static bool parseZArray(Thread& th, P<Code>& code)
{
	ParsingWhat pw(th, parsingArray);
	th.getc();
	th.getc();
	P<Code> code2 = new Code(8);
	parseItemList(th, code2, ']');

	if (code2->size()) {
		code2->add(opReturn, 0.);
		code2->shrinkToFit();
		code->add(opNewZList, V(code2));
	} else {
		code->add(opPushImmediate, V(vm._nilz));
	}
			
	return true;
}

bool parseItemList(Thread& th, P<Code>& code, int endbrace)
{	
	//ScopeLog sl("parseItemList");
	
	while (1) {
		skipSpace(th);
		int c = th.c();
		if (c == endbrace) break;
		if (!parseElem(th, code)) {
			if (endbrace == ']') syntaxError("expected ']'");
			if (endbrace == '}') syntaxError("expected '}'");
			if (endbrace == ')') syntaxError("expected ')'");
		}
	}
	th.getc(); // skip end brace
	
	return true;
}


bool parseSymbol(Thread& th, P<String>& result)
{
	//ScopeLog sl("parseSymbol");
	skipSpace(th);
	const char* start = th.curline();
	int c = th.getc();

	if (th.c() == '=') {
		if (c == '<' || c == '>' || c == '=') {
			th.getc();
		}
	} else if (c != '=') {
		while(!endOfWord(c)) {
			c = th.getc();
		}
		th.unget(1);
	}
	size_t len = th.curline() - start;
	if (len == 0) return false;

	th.toToken(start, (int)len);
	
	result = getsym(th.token);

	return true;
}

static void bindVar(Thread& th, P<String> const& name, P<Code>& code)
{
	V val;
	size_t index;
	int scope = th.mCompileScope->bindVar(th, name, index);

	V vname(name);
	if (scope == scopeWorkspace) {
		// compiling at top level
		code->add(opBindWorkspaceVar, vname);
	} else {
		val.i = index;
		code->add(opBindLocal, val);
	}
}

static void bindVarFromList(Thread& th, P<String> const& name, P<Code>& code)
{
	V val;
	size_t varIndex;
	int scope = th.mCompileScope->bindVar(th, name, varIndex);

	if (scope == scopeWorkspace) {
		// compiling at top level
		V vname(name);
		code->add(opBindWorkspaceVarFromList, vname);
	} else {
		val.i = varIndex;
		code->add(opBindLocalFromList, val);
	}
}

bool parseWord(Thread& th, P<Code>& code)
{
	//ScopeLog sl("parseWord");
	P<String> name;

	if (!parseSymbol(th, name)) return false;
		
	if (strcmp(name->cstr(), "=")==0) {

		skipSpace(th);		
		if (th.c() == '(') {
			th.getc();
			// parse multiple assign
			std::vector<P<String>> names;
			{
				P<String> name2;
				while (parseSymbol(th, name2)) {
					names.push_back(name2);
				}
			}
			if (names.size() == 0) {
				syntaxError("expected a name after '= ('\n");
			}
			for (int64_t i = names.size()-1; i>=0; --i) {
				bindVar(th, names[i], code);
			}
			skipSpace(th);
			if (th.c() != ')') {
				syntaxError("expected ')' after '= ('\n");
			}
			th.getc();
			code->add(opNone, 0.);
		} else if (th.c() == '[') {
			th.getc();
			// parse assign from array
			std::vector<P<String>> names;
			{
				P<String> name2;
				while (parseSymbol(th, name2)) {
					names.push_back(name2);
				}
			}
			if (names.size() == 0) {
				syntaxError("expected a name after '= ['\n");
			}
			for (size_t i = 0; i<names.size(); ++i) {
				bindVarFromList(th, names[i], code);
			}
			skipSpace(th);
			if (th.c() != ']') {
				syntaxError("expected ']' after '= ['\n");
			}
			th.getc();
			code->add(opNone, 0.);
		} else {
			P<String> name2;
			if (!parseSymbol(th, name2)) {
				syntaxError("expected a name after '='\n");
			}
			bindVar(th, name2, code);
		}
	} else {
		V val;
		size_t index;
		int scope = th.mCompileScope->indirectLookup(th, name, index, val);
		V vname(name);
		switch (scope) {
			case scopeLocal :
				val.i = index;
				code->add(opCallLocalVar, val);
				break;
			case scopeFunVar :
				val.i = index;
				code->add(opCallFunVar, val);
				break;
			case scopeBuiltIn :
				code->add(opCallImmediate, val);
				break;
			case scopeWorkspace :
				code->add(opCallWorkspaceVar, vname);
				break;
			default :
				post("\"%s\" is an undefined word\n", name->cstr());
				syntaxError("undefined word");
				
		}

	}
	
	return true;
}

static bool parseString(Thread& th, P<Code>& code)
{
	//ScopeLog sl("parseString");
	ParsingWhat pw(th, parsingString);
	
	V string = parseString(th);	
	code->add(opPushImmediate, string);

	return true;
}

static String* parseString(Thread& th)
{
	if (th.c() != '"') return nullptr;

	ParsingWhat pw(th, parsingString);
	th.getc();
	int c = th.getc();
	std::string str;
		
	while (true) {
		if (c == 0) {
			syntaxError("end of input in string");
		} else if (c == '\\' && th.c() == '\\') {
			th.getc();
			c = th.getc();
			switch (c) {
				case 'n' : str += '\n'; break;
				case 'r' : str += '\r'; break;
				case 'f' : str += '\f'; break;
				case 'v' : str += '\v'; break;
				case 't' : str += '\t'; break;
				default : str += c; break;
			}
			c = th.getc();
		} else if (c == '"') {
			if (th.c() == '"') {
				c = th.getc();
				str += '"';
			} else {
				break;
			}
		} else {
			str += c;
			c = th.getc();
		}
	}
	
	return new String(str.c_str());
}

bool parseElem(Thread& th, P<Code>& code)
{	
	skipSpace(th);
	int c = th.c();
	if (c == 0)
		return false;
	if (c == ']' || c == ')' || c == '}') {
		post("unexpected '%c'.\n", c);
		throw errSyntax;
	}
	if (c == '@')
		 return parseEachOp(th, code);
	if (c == '(')
		 return parseParens(th, code);
	if (c == '[')
		 return parseArray(th, code);
	if (c == '{')
		 return parseNewForm(th, code);
	if (c == '\\')
		 return parseLambda(th, code);
	if (c == '"')
		return parseString(th, code);
	if (c == '\'') 
		return parseQuote(th, code);
	if (c == '`') 
		return parseBackquote(th, code);
	if (c == ',') 
		return parseComma(th, code);
	if (c == ':')
		return parseColon(th, code);

	if (c == '0' && th.d() == 'x') 
		return parseHexNumber(th, code) || parseWord(th, code);

	if (isdigit(c) || c == '+' || c == '-')
		return parseNumber(th, code) || parseWord(th, code);
        
    if (c == 'p' && th.d() == 'i')
		return parseNumber(th, code) || parseWord(th, code);

	if (c == '.')
		return parseNumber(th, code) || parseDot(th, code);


	if (c == '#') {
		int d = th.d();
		if (!d) syntaxError("end of input after '#'");
		if (d == '[') {
			return parseZArray(th, code);
		} else {
			return false;
		}
	}
	
	return parseWord(th, code);
}


bool parseElems(Thread& th, P<Code>& code)
{
	code = new Code(8);
	while (1) {
		if (!parseElem(th, code)) break;
	}
	
	code->add(opReturn, 0.);
	code->shrinkToFit();
		
	return true;
}

/////////////////////////

#pragma mark PRINTI

#include <ostream>

/////////////////////////         1         2         3         4         5         6
/////////////////////////1234567890123456789012345678901234567890123456789012345678901234
const char* s64Spaces = "                                                                ";
const int kNumSpaces = 64;

static void printSpaces(std::ostream& ost, int n)
{
	while (n >= kNumSpaces) {
		ost << s64Spaces;
		n -= kNumSpaces;
	}
	if (n) {
		ost << (s64Spaces + kNumSpaces - n);
	}
}

void printi(std::ostream& ost, int indent, const char* fmt, ...)
{
	printSpaces(ost, indent);
    ssize_t final_n, n = 256;
    std::string str;
    std::unique_ptr<char[]> formatted;
    va_list ap;
    while(1)
	{
        formatted.reset(new char[n]); /* wrap the plain char array into the unique_ptr */
        strcpy(&formatted[0], fmt);
        va_start(ap, fmt);
        final_n = vsnprintf(&formatted[0], n, fmt, ap);
        va_end(ap);
        if (final_n < 0)
			return; // error
		if (n <= final_n) {
            n = final_n;
        } else {
			ost << formatted.get();
			return;
		}
    }
}

void prints(std::ostream& ost, const char* fmt, ...)
{
    ssize_t final_n, n = 256;
    std::string str;
    std::unique_ptr<char[]> formatted;
    va_list ap;
    while(1)
	{
        formatted.reset(new char[n]); /* wrap the plain char array into the unique_ptr */
        strcpy(&formatted[0], fmt);
        va_start(ap, fmt);
        final_n = vsnprintf(&formatted[0], n, fmt, ap);
        va_end(ap);
        if (final_n < 0)
			return; // error
		if (n <= final_n) {
            n = final_n;
        } else {
			ost << formatted.get();
			return;
		}
    }
}
//////////////////////////////////

