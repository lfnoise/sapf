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

#include "VM.hpp"
#include "Opcode.hpp"
#include "Parser.hpp"
#include "MultichannelExpansion.hpp"
#include "elapsedTime.hpp"
#include <stdexcept>
#include <limits.h>

VM vm;

pthread_mutex_t gHelpMutex = PTHREAD_MUTEX_INITIALIZER;

std::atomic<int32_t> randSeedCounter = 77777;

uint64_t timeseed()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	int32_t counter = ++randSeedCounter;
	return Hash64(tv.tv_sec) + Hash64(tv.tv_usec) + Hash64(counter);
}


Thread::Thread()
    :rate(vm.ar), stackBase(0), localBase(0),
	mWorkspace(new GForm()),
    parsingWhat(parsingWords),
    fromString(false),
    line(NULL)
{
	rgen.init(timeseed());
}

Thread::Thread(const Thread& inParent)
    :rate(inParent.rate), stackBase(0), localBase(0),
    mWorkspace(inParent.mWorkspace),
    parsingWhat(parsingWords),
    fromString(false),
    line(NULL)
{
	rgen.init(timeseed());
}

Thread::Thread(const Thread& inParent, P<Fun> const& inFun)
    :rate(vm.ar), stackBase(0), localBase(0),
    fun(inFun),
    mWorkspace(inParent.mWorkspace),
    parsingWhat(parsingWords),
    fromString(false),
    line(NULL)
{
	rgen.init(timeseed());
}

Thread::~Thread() {}

//////////////////////

static void inherit_(Thread& th, Prim* prim)
{
	V vparent = th.pop();
	P<Form> form = asParent(th, vparent);
	th.push(form);
}

P<Form> extendFormByOne(Thread& th, P<Form> const& parent, P<TableMap> const& tmap, Arg value)
{	
	P<Table> table = new Table(tmap);
	P<Form> form = new Form(table, parent);
	table->put(0, value);
	return form;
}

static void newForm_(Thread& th, Prim* prim)
{
	TableMap* tmap = (TableMap*)th.pop().o();
	size_t numArgs = tmap->mSize;
	
	V* args = &th.top() - numArgs + 1;
	
	P<Table> table = new Table(tmap);
	V vparent = args[-1];
	P<Form> parent = asParent(th, vparent);
	
	P<Form> form = new Form(table, parent);
	
	for (size_t i = 0; i < numArgs; ++i) {
		table->put(i, args[i]);
	}
	
	th.popn(numArgs+1);
	
	th.push(form);
}

static void newVList_(Thread& th, Prim* prim)
{
	size_t n = th.stackDepth();
	
	P<List> seq;
	if (n == 0) {
		seq = vm._nilv;
	} else {
		seq = new List(itemTypeV, n);
		V* ssp = &th.top() - n + 1;
		
		for (size_t i = 0; i < n; ++i) {
			seq->add(ssp[i]);
		}
	}
	
	th.popn(n);
	th.push(seq);		
}

static void newZList_(Thread& th, Prim* prim)
{
	size_t n = th.stackDepth();
	
	P<List> seq;
	if (n == 0) {
		seq = vm._nilz;
	} else {
		seq = new List(itemTypeZ, n);
		V* ssp = &th.top() - n + 1;
		
		for (size_t i = 0; i < n; ++i) {
			seq->add(ssp[i]);
		}
	}
	
	th.popn(n);
	
	th.push(seq);		
}

//////////////////////


VM::VM()
	:
	prelude_file(NULL),
	log_file(NULL),
	_ee(0),
		
	printLength(20),
	printDepth(8),
	
	ar(kDefaultSampleRate, kDefaultZBlockSize),
	kr(ar, kDefaultControlBlockSize),
	
	VblockSize(kDefaultVBlockSize),
	
#if COLLECT_MINFO
	totalRetains(0),
	totalReleases(0),
	totalObjectsAllocated(0),
	totalObjectsFreed(0),
	totalSignalGenerators(0),
	totalStreamGenerators(0)
#endif
{
	initElapsedTime();
	
	_ee = new Form(0, NULL);
		
	builtins = new GTable();
	
	// add built in funs
		
	_nilz = new List(itemTypeZ);
	_nilv = new List(itemTypeV);
	
	_anilz = _nilz->mArray;
	_anilv = _nilv->mArray;
	
	newForm = new Prim(newForm_, 0., 0, 1, NULL, NULL);
	inherit = new Prim(inherit_, 0., 0, 1, NULL, NULL);

	newVList = new Prim(newVList_, 0., 0, 1, NULL, NULL);
	newZList = new Prim(newZList_, 0., 0, 1, NULL, NULL);
}

VM::~VM()
{
}

#if USE_LIBEDIT
static const char* prompt(EditLine *e) 
{
  return "sapf> ";
}
static const char* promptParen(EditLine *e) 
{
  return "(sapf> ";
}
static const char* promptSquareBracket(EditLine *e) 
{
  return "[sapf> ";
}
static const char* promptCurlyBracket(EditLine *e) 
{
  return "{sapf> ";
}
static const char* promptLambda(EditLine *e) 
{
  return "\\sapf> ";
}
static const char* promptString(EditLine *e) 
{
  return "\"sapf> ";
}
#endif

void Thread::getLine()
{	
	if (fromString) return;
	switch (parsingWhat) {
		default: case parsingWords : el_set(el, EL_PROMPT, &prompt); break;
		case parsingString : el_set(el, EL_PROMPT, &promptString); break;
		case parsingParens : el_set(el, EL_PROMPT, &promptParen); break;
		case parsingLambda : el_set(el, EL_PROMPT, &promptLambda); break;
		case parsingArray : el_set(el, EL_PROMPT, &promptSquareBracket); break;
		case parsingEnvir : el_set(el, EL_PROMPT, &promptCurlyBracket); break;
	}
	line = el_gets(el, &linelen);
	linepos = 0;
	if (line == NULL || strncmp(line, "quit", 4)==0 || strncmp(line, "..", 2)==0) { line = NULL; throw errUserQuit; }
	if (line && linelen) {
		history(myhistory, &ev, H_ENTER, line);
		history(myhistory, &ev, H_SAVE, historyfilename);
		if (logfilename) {
			FILE* logfile = fopen(logfilename, "a");
			logTimestamp(logfile);
			fwrite(line, 1, strlen(line), logfile);
			fclose(logfile);
		}
	}
}

void Thread::logTimestamp(FILE* logfile)
{
	timeval tv;
	gettimeofday(&tv, NULL);
	if (previousTimeStamp == 0 || tv.tv_sec - previousTimeStamp > 3600) {
		previousTimeStamp = tv.tv_sec;
		char date[32];
		ctime_r(&tv.tv_sec, date);
		fprintf(logfile, ";;;;;;;; %s", date);
		fflush(logfile);
	}
}

char Thread::getc() { 
	if (fromString) {
		if (line == NULL) return 0;
		return line[linepos++];
	} else {
		while (1) {	
			if (line == NULL) {
				getLine();
				if (line == NULL || linelen == 0) return 0;
			} else if (line[linepos] == 0) {
				if (parsingWhat == parsingWords) {
					return 0;
				} else {
					getLine();
					if (linelen == 0 || strcmp(line, "\n") == 0) {
						line = NULL;
						continue;
					}
					if (line == NULL || linelen == 0) return 0;
				}
			} else {
				return line[linepos++];
			}
		}
	}
	return 0; // never gets here, but compiler too dumb to figure this out.
}

void Thread::repl(FILE* infile, const char* inLogfilename)
{
	Thread& th = *this;

	logfilename = inLogfilename;
	
	previousTimeStamp = 0;

#if USE_LIBEDIT
	el = el_init("sc", stdin, stdout, stderr);
	el_set(el, EL_PROMPT, &prompt);
	el_set(el, EL_EDITOR, "emacs");
	el_set(el, EL_BIND, "-s", "\t", "    ", NULL);

	myhistory = history_init();
	if (myhistory == 0) {
		post("history could not be initialized\n");
		return;
	}

	const char* envHistoryFileName = getenv("SAPF_HISTORY");
	if (envHistoryFileName) {
		snprintf(historyfilename, PATH_MAX, "%s", envHistoryFileName);
	} else {
		const char* homeDir = getenv("HOME");
		snprintf(historyfilename, PATH_MAX, "%s/sapf-history.txt", homeDir);
	}
	history(myhistory, &ev, H_SETSIZE, 800);
	history(myhistory, &ev, H_LOAD, historyfilename);
	history(myhistory, &ev, H_SETUNIQUE, 1);
	el_set(el, EL_HIST, history, myhistory);
#endif
	
	fflush(infile);
	bool running = true;

	post("Type 'helpall' to get a list of all built-in functions.\n");
	post("Type 'quit' to quit.\n");
	
	do {
		try {
			if (stackDepth()) {
				printStack();
				post("\n");
			}
		} catch (int err) {
			if (err <= -1000 && err > -1000 - kNumErrors) {
				post("\nerror: %s\n", errString[-1000 - err]);
			} else {
				post("\nerror: %d\n", err);
			}
		} catch (std::bad_alloc& xerr) {
			post("\nnot enough memory\n");
		} catch (...) {
			post("\nunknown error\n");
		}
				
		try {
			// PARSE
			{
				P<Fun> compiledFun;
				if (compile(NULL, compiledFun, true)) {
				// EVAL
					compiledFun->runREPL(th);
				}
			}
		} catch (V& v) {
            post("error: ");
            v.print(th);
			post("\n");
		} catch (int err) {
			if (err == errUserQuit) {
				post("good bye\n");
				running = false;
			} else if (err <= -1000 && err > -1000 - kNumErrors) {
				post("error: %s\n", errString[-1000 - err]);
			} else {
				post("error: %d\n", err);
			}
		} catch (std::bad_alloc& xerr) {
			post("not enough memory\n");
		} catch (...) {
			post("unknown error\n");
		}

	} while (running);
	

#if USE_LIBEDIT
	history(myhistory, &ev, H_SAVE, historyfilename);
	history_end(myhistory);
	el_end(el);
#endif
}


template <typename T>
class AutoFree
{
	T* p;
public:
	AutoFree(T* _p) : p(_p) {}
	~AutoFree() { free(p); }
	
	T* operator()() { return p; }
	T* operator*() { return p; }
	T* operator->() { return p; }
};

void loadFile(Thread& th, const char* filename)
{
    post("loading file '%s'\n", filename);
	FILE* f = fopen(filename, "r");
	if (!f) {
		post("could not open '%s'\n", filename);
		return;
	}

	fseek(f, 0, SEEK_END);
	int64_t fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	
	AutoFree<char> buf = (char*)malloc(fileSize + 1);
	const char* p = buf();
	fread(buf(), 1, fileSize, f);
	buf()[fileSize - 1] = 0;
		
	try {
		{
			P<Fun> compiledFun;
			if (th.compile(p, compiledFun, true)) {
				post("compiled OK.\n");
				compiledFun->run(th);
				post("done loading file\n");
			}
		}
    } catch (V& v) {
        post("error: ");
        v.print(th);
        post("\n");
	} catch (int err) {
		if (err <= -1000 && err > -1000 - kNumErrors) {
			post("error: %s\n", errString[-1000 - err]);
		} else {
			post("error: %d\n", err);
		}
	} catch (std::bad_alloc& xerr) {
		post("not enough memory\n");
	} catch (...) {
		post("unknown error\n");
	}
}

void Thread::printStack()
{
	bool between = false;
	size_t n = stackDepth();
	for (size_t i = 0; i < n; ++i) {
		V* s = &stack[stackBase+i];
		if (between) post(" ");
		else between = true;
		std::string cppstring;
		s->print(*this, cppstring);
		post("%s", cppstring.c_str());
	}
}

void Thread::printLocals()
{
	bool between = false;
	size_t n = numLocals();
	for (size_t i = 0; i < n; ++i) {
		V* s = &local[localBase+i];
		if (between) post(" ");
		else between = true;
		std::string cppstring;
		s->print(*this, cppstring);
		post("%s", cppstring.c_str());
	}
}

void VM::setSampleRate(double inSampleRate)
{
	ar = Rate(inSampleRate, ar.blockSize);
	kr = Rate(ar, kDefaultControlBlockSize);
}

V VM::def(Arg key, Arg value)
{
	builtins->putImpure(key, value); 
    V dummy;
    assert(builtins->getInner(key, dummy));
	return value;
}

V VM::def(const char* name, Arg value)
{
	def(V(getsym(name)), value);
	return value;
}

V VM::def(const char* name, int takes, int leaves, PrimFun pf, const char* help, Arg value, bool setNoEach)
{
	V aPrim = new Prim(pf, value, takes, leaves, name, help);
	def(name, aPrim);
	
	if (setNoEach) aPrim.SetNoEachOps();
	
	if (help)
		addBifHelp(name, aPrim.GetAutoMapMask(), help);
		
	return aPrim;
}

V VM::defmcx(const char* name, int numArgs, PrimFun pf, const char* help, Arg value)
{
	V aPrim = new Prim(pf, value, numArgs, 1, name, help);
	aPrim = mcx(numArgs, aPrim, name, help);
	def(name, aPrim);
		
	addBifHelp(name, aPrim.GetAutoMapMask(), help);
	return aPrim;
}

V VM::defautomap(const char* name, const char* mask, PrimFun pf, const char* help, Arg value)
{
	int numArgs = (int)strlen(mask);
	V aPrim = new Prim(pf, value, numArgs, 1, name, help);
	aPrim = automap(mask, numArgs, aPrim, name, help);
	def(name, aPrim);
		
	addBifHelp(name, aPrim.GetAutoMapMask(), help);
	return aPrim;
}


#pragma mark STACK

#define POPREFTYPEDEF(TYPE) \
P<TYPE> Thread::pop##TYPE(const char* msg) \
{ \
	V v = pop(); \
    ApplyIfFun(v); \
	if (!v.is##TYPE()) wrongType(msg, #TYPE, v); \
	return reinterpret_cast<TYPE*> (v.o()); \
}

#define POPTYPEDEF(TYPE) \
P<TYPE> Thread::pop##TYPE(const char* msg) \
{ \
	V v = pop().deref(); \
    ApplyIfFun(v); \
	if (!v.is##TYPE()) wrongType(msg, #TYPE, v); \
	return reinterpret_cast<TYPE*> (v.o()); \
}

#define POPFUNTYPEDEF(TYPE) \
P<TYPE> Thread::pop##TYPE(const char* msg) \
{ \
	V v = pop().deref(); \
	if (!v.is##TYPE()) wrongType(msg, #TYPE, v); \
	return reinterpret_cast<TYPE*> (v.o()); \
}

POPREFTYPEDEF(Ref);
POPTYPEDEF(ZRef);
POPTYPEDEF(String);
POPTYPEDEF(List);
POPFUNTYPEDEF(Fun);
POPTYPEDEF(Form);

void Thread::ApplyIfFun(V& v)
{
    if (v.isFunOrPrim()) {
        SaveStack ss(*this);
        v.apply(*this);
        v = pop();
    }
}


V Thread::popZInList(const char* msg)
{
	V p = pop();
	if (p.isRef()) p = p.deref();
    ApplyIfFun(p);
	if (!p.isZIn() && !p.isVList()) {
		wrongType(msg, "Real or Signal or List of Reals or Signals", p);
	}
	return p;
}

V Thread::popZIn(const char* msg)
{
	V p = pop();
	if (p.isRef() || p.isZRef()) p = p.deref();
    ApplyIfFun(p);
	if (!p.isZIn()) {
		wrongType(msg, "Real or Signal", p);
	}
	return p;
}

V Thread::popValue()
{
	V p = pop().deref();
    ApplyIfFun(p);
	return p;
}

P<List> Thread::popVList(const char* msg)
{
	V v = pop().deref();
    ApplyIfFun(v);
	if (!v.isVList()) {
		wrongType(msg, "Stream", v);
	}
	return (List*)v.o();
}

P<List> Thread::popZList(const char* msg)
{
	V v = pop().deref();
    ApplyIfFun(v);
	if (!v.isZList()) {
		wrongType(msg, "Signal", v);
	}
	return (List*)v.o();
}

int64_t Thread::popInt(const char* msg)
{
	V v = pop().deref();
    ApplyIfFun(v);
	if (!v.isReal()) wrongType(msg, "Real", v);
	
	double f = v.f;
	int64_t i;
	if (f >= (double)LLONG_MAX) i = LLONG_MAX;
	else if (f <= (double)LLONG_MIN) i = LLONG_MIN;
	else i = (int64_t)f;
	return i;
}

double Thread::popFloat(const char* msg)
{
	V v = pop().deref();
    ApplyIfFun(v);
	if (!v.isReal()) wrongType(msg, "Float", v);
	return v.f;
}

void Thread::tuck(size_t n, Arg v)
{
	stack.push_back(V(0.));
	V* sp = &stack.back();
	
	for (size_t i = 0; i < n; ++i)
		sp[-i] = sp[-i-1];
	
	sp[-n] = v;
}

///////////////////////////////////////

bool Thread::compile(const char* inString, P<Fun>& compiledFun, bool inTopLevel)
{	
	Thread& th = *this;
	SaveCompileScope scs(th);
	if (inTopLevel) {
		mCompileScope = new TopCompileScope();
	} else {
		mCompileScope = new InnerCompileScope(new TopCompileScope());
	}
	
	P<Code> code;
	setParseString(inString);
	getLine();
	bool ok = parseElems(th, code);
	if (!ok || !code) {
		post("parse error. %d\n", ok);
		return false;
	}
		
	compiledFun = new Fun(*this, new FunDef(*this, code, 0, th.mCompileScope->numLocals(), th.mCompileScope->numVars(), NULL));
	
	return true;
}

int TopCompileScope::directLookup(Thread& th, P<String> const& inName, size_t& outIndex, V& outBuiltIn)
{
	int scope = CompileScope::directLookup(th, inName, outIndex, outBuiltIn);
	if (scope != scopeUndefined) return scope;

	for (size_t i = 0; i < mWorkspaceVars.size(); ++i) {
		if (mWorkspaceVars[i].mName() == inName()) {
			return scopeWorkspace;
		}
	}
	
	V value;
	if (th.mWorkspace->get(th, inName(), value)) {
		return scopeWorkspace;
	} else if (vm.builtins->get(th, inName, value)) {
		outBuiltIn = value;
		return scopeBuiltIn;
	}

	return scopeUndefined;
}


int CompileScope::directLookup(Thread& th, P<String> const& inName, size_t& outIndex, V& outBuiltIn)
{
	for (size_t i = 0; i < mLocals.size(); ++i) {
		if (mLocals[i].mName() == inName()) {
			outIndex = i;
			return scopeLocal;
		}
	}
	for (size_t i = 0; i < mVars.size(); ++i) {
		if (mVars[i].mName() == inName()) {
			outIndex = i;
			return scopeFunVar;
		}
	}
	
	return scopeUndefined;
}


int TopCompileScope::indirectLookup(Thread& th, P<String> const& inName, size_t& outIndex, V& outBuiltIn)
{
	return directLookup(th, inName, outIndex, outBuiltIn);
}

int InnerCompileScope::indirectLookup(Thread& th, P<String> const& inName, size_t& outIndex, V& outBuiltIn)
{
	int scope = directLookup(th, inName, outIndex, outBuiltIn);
	if (scope != scopeUndefined) 
		return scope;

	size_t outerIndex;
	scope = mNext->indirectLookup(th, inName, outerIndex, outBuiltIn);
	if (scope == scopeUndefined) 
		return scopeUndefined;
	
	if (scope == scopeLocal || scope == scopeFunVar) {
		VarDef def;
		def.mName = inName;
		def.mIndex = mVars.size();
		def.mFromScope = scope;
		def.mFromIndex = outerIndex;
		outIndex = mVars.size();
		mVars.push_back(def);
		return scopeFunVar;
	}
	
	return scope;
}

int TopCompileScope::bindVar(Thread& th, P<String> const& inName, size_t& outIndex)
{
	V v;
	int scope = directLookup(th, inName, outIndex, v);
	if (scope != scopeUndefined && scope != scopeBuiltIn) 
		return scope; // already defined
		
	WorkspaceDef def;
	def.mName = inName;
	mWorkspaceVars.push_back(def);

	return scopeWorkspace;
}

int CompileScope::innerBindVar(Thread& th, P<String> const& inName, size_t& outIndex)
{
	V v;
	int scope = directLookup(th, inName, outIndex, v);
	if (scope == scopeFunVar) {
		post("Name %s is already in use in this scope as a free variable.\n", inName->cstr());
		throw errSyntax;
	}
	
	if (scope == scopeUndefined) {		
		LocalDef def;
		def.mName = inName;
		outIndex = def.mIndex = mLocals.size();
		mLocals.push_back(def);
	}
	return scopeLocal;
}

int InnerCompileScope::bindVar(Thread& th, P<String> const& inName, size_t& outIndex)
{
	return innerBindVar(th, inName, outIndex);
}

CompileScope* ParenCompileScope::nextNonParen() const
{
	CompileScope* scope = mNext();
	while (scope->isParen()) {
		scope = scope->mNext();
	}
	return scope;
}

int ParenCompileScope::directLookup(Thread& th, P<String> const& inName, size_t& outIndex, V& outBuiltIn)
{
	return mNext->directLookup(th, inName, outIndex, outBuiltIn);
}

int ParenCompileScope::indirectLookup(Thread& th, P<String> const& inName, size_t& outIndex, V& outBuiltIn)
{
	return mNext->indirectLookup(th, inName, outIndex, outBuiltIn);
}

int ParenCompileScope::innerBindVar(Thread& th, P<String> const& inName, size_t& outIndex)
{
	return mNext->innerBindVar(th, inName, outIndex);
}

int ParenCompileScope::bindVar(Thread& th, P<String> const& inName, size_t& outIndex)
{
	return mNext->innerBindVar(th, inName, outIndex);
}

//////////////////////




