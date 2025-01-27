#include "stdafx.h"
#include "tchannel.h"
#include "tools.h"
#include "rconnect_interface.h"

DWORD g_main_TID = 0;

//#undef ERROR

#define Win32
#include <Rembedded.h>
#include <R_ext/RStartup.h>
#include <R_ext/Parse.h>

//__declspec(dllimport) extern int UserBreak;
extern "C" __declspec(dllimport) extern int R_SignalHandlers;
extern "C" void run_Rmainloop();

extern char dllName[MAX_PATH];
extern wchar_t arcgis_path[MAX_PATH];


extern const rconnect_interface* current_connect;
bool g_InProc = false;

namespace GNU_GPL
{
//private:
struct local
{
  static void run_loop(void* b)
  {
    if (!initialize(*(bool*)b))
    {
      tchannel::value p(tchannel::R_PROMPT, NULL);
      tchannel& channel = tchannel::singleton();
      channel.from_thread.push(p);
      return;
    }
    ::run_Rmainloop();
  }

  //public:
  static void showMessage(const char *msg)
  {
    ::MessageBox(NULL, _bstr_t(msg), NULL, MB_OK);
  }
  static void busy(int which)
  {
    //if (HIWORD(GetAsyncKeyState(VK_ESCAPE)))
    //    UserBreak = 1;
    //::SetCursor(::LoadCursor(NULL, MAKEINTRESOURCE(IDC_WAIT)));
  }

  static void writeOutEx(const char *bufUtf8, int len, int otype)
  {
    if (len == 0) return;
    if (current_connect)
    {
      if (otype == 1) otype = 0; //print message() as plain text
      if (g_main_TID != GetCurrentThreadId())
      {
        //while(g_channel.from_thread.size() > 1000)
        //  Sleep(10);
        try{
          tchannel::value p(tchannel::R_TXT_OUT, new std::wstring(1, otype ? L'2' : L'0'));
          p.data_str->append(tools::fromUtf8(bufUtf8));
          tchannel& channel = tchannel::singleton();
          channel.from_thread.push(p);
        }catch(...)
        {
        }
      }
      else
        current_connect->print_out(tools::fromUtf8(bufUtf8).c_str(), otype);
    }
    if (otype)
    {
      ::OutputDebugStringA(bufUtf8);
    }
  }

  static ParseStatus pre_eval2arc_cmd(const char *statement)
  {
    tools::protect pt;
    SEXP cmd = pt.add(tools::newVal(statement));
    //pre-parse multiline
    ParseStatus status = PARSE_NULL;
    SEXP expr_cmd = pt.add(R_ParseVector(cmd, -1, &status, R_NilValue));
    //SEXP expr_cmd = foo(pt, cmd, status);
    if (status == PARSE_INCOMPLETE) 
      return status;

    Rf_defineVar(Rf_install(".arc_cmd"), cmd, R_GlobalEnv);
    return status;
  }
  
  static int readConsole(const char *prompt, char *buf, int len, int addtohistory)
  {
    std::wstring* ptr = new std::wstring(tools::fromUtf8(prompt));
    tchannel::value p(tchannel::R_PROMPT, ptr);
    tchannel& channel = tchannel::singleton();
    channel.from_thread.push(p);

    if (xwait(channel.to_thread.handle(), 0) == -1)
      return 0;

    if (channel.to_thread.pop(p, 0))
    {
      ATLASSERT(p.type == tchannel::R_CMD);
      int cmd_type = p.data_int;
#if 0
      const static char eval_str[] = "tryCatch("
      "eval(parse(text=.arc_cmd)),"
      // we have to parse again to show syntax error
      //"eval(arc$.cmd),"
      "error=function(e) invisible(.Call('arc_error',geterrmessage()))"
      ",finally=.arc_cmd<-NULL"
      ")\n";
#else
      const static char eval_str[] = 
        "tryCatch("
           "withRestarts("
              "withCallingHandlers("
                 "eval(parse(text=.arc_cmd)),"
                 "error=function(e)invokeRestart('big.bada.boom', e, sys.call(sys.parent()-2L)),"
                 "warning=function(w){.Call('arc_warning', conditionMessage(w), PACKAGE='rarcproxy'); invokeRestart('muffleWarning')}"
              "),"
              "big.bada.boom=function(e, calls){"
                "trace.back <- lapply(calls, deparse)\n"
                "if (trace.back[1] == 'eval') msg <- conditionMessage(e) "
                "else if (trace.back[1] == '') msg<-conditionMessage(e) "
                "else msg <-paste0('Error in ', trace.back[1], ' : ', conditionMessage(e))\n"
                "invisible(.Call('arc_error', msg, PACKAGE='rarcproxy'))"
              "}"
           "),"
           "finally=rm(.arc_cmd, envir=.GlobalEnv)"
         ")\n";
#endif
      if (cmd_type)
        strncpy_s(buf, len, eval_str, _countof(eval_str));
      else
      {
        buf[0] = '\n';
        buf[1] = 0;
      }
      return 1;
    }
    return 0;
  }

  static int yes_no_cancel(const char *msg)
  {
    return ::MessageBox(NULL, _bstr_t(msg), L"R scripting", MB_YESNOCANCEL);
  }

  static void myCallBack()
  {
    /// called during i/o, eval, graphics in ProcessEvents
    //OutputDebugStringA(".");
    if (UserBreak == 0 && isCancel())
    {
      UserBreak = 1;
    }
  }

//--------------
  static ParseStatus evaluate_string(LPCSTR statement)
  {
    std::string exec_code(statement);

    //remove \r
    size_t pos = 0;
    while ((pos = exec_code.find('\r', pos)) != std::string::npos)
      exec_code.at(pos++) = ' ';

    ParseStatus status; 
    SEXP cmdSexp = NULL;
    tools::protect pt;
    cmdSexp = pt.add(tools::newVal(exec_code));
    SEXP env = R_GlobalEnv;

    SEXP cmdexpr = pt.add(R_ParseVector(cmdSexp, -1, &status, R_NilValue));
    if (status != PARSE_OK) 
      return PARSE_NULL;
    
    int eError = 0;
    SEXP res = NULL;
    if (TYPEOF(cmdexpr) == EXPRSXP)
    {
      for(int i = 0, n = Rf_length(cmdexpr); i < n; i++)
      {
        res = R_tryEval(VECTOR_ELT(cmdexpr, i), env, &eError);
        if (eError) 
        {
          break;
        }
        //Rf_PrintValue(res);
      }
    }
    else
      res = R_tryEval(cmdexpr, env, &eError);

    if (eError)
    {
      std::string error;
      tools::copy_to(res, error);
      return PARSE_NULL;
    }

    return status;
  }
  
  static bool process_message(tchannel::value &p)
  {
    switch (p.type)
    {
      case tchannel::FN_CALL:
      {
        const fn_struct *it = p.data_fn;
        tchannel::value p2(tchannel::FN_CALL_RET, it->call());
        tchannel& channel = tchannel::singleton();
        channel.to_thread.push(p2);
      }break;
      case tchannel::R_PROMPT:
      {
        ATLASSERT(current_connect);
        const_cast<rconnect_interface*>(current_connect)->new_prompt(p.data_str->c_str());
      }return true;//PARSE_OK;

      case tchannel::R_TXT_OUT:
      {
        //std::auto_ptr<std::wstring> str((std::wstring*)p.data);
        std::wstring* str = p.data_str;
        ATLASSERT(current_connect);
        const wchar_t* ptr = str->c_str();
        current_connect->print_out(ptr + 1, ptr[0] - L'0');
      }break;
      default:
        ATLASSERT(0); //unknown command???
        return true;//PARSE_ERROR;
    }
    return false;
  }

  static inline int xwait(HANDLE h1, HWND hw = (HWND)-1)
  {
    DWORD n = 1;
    HANDLE h[] = {h1};

    DWORD wakemask = QS_POSTMESSAGE|QS_TIMER|QS_PAINT|
                  //QS_KEY|   
                  QS_MOUSE|QS_POSTMESSAGE|QS_SENDMESSAGE;

    while (true)
    {
      DWORD dw = ::MsgWaitForMultipleObjects(n, h, FALSE, INFINITE, wakemask);
      if ((dw - WAIT_OBJECT_0) < n)
        return dw - WAIT_OBJECT_0;
      dw = ::WaitForMultipleObjects(n, h, FALSE, 0);
      if ((dw - WAIT_OBJECT_0) < n)
        return dw - WAIT_OBJECT_0;

      MSG msg;
      while (::PeekMessage(&msg, hw, 0, 0, PM_REMOVE))
      {
        if (msg.message == WM_QUIT)
          return -1;
        ::DispatchMessage(&msg);
      }
    }
  }

  static bool initialize(bool bInteractive = true)
  {
    const char* dllVer = getDLLVersion();
    if (!dllVer)
      return false;
    //check major relese
    if (dllVer[0] != R_MAJOR[0])
      return false;
    // strict check
    // if (strncmp(dllVer, RVERSION_DLL_BUILD, 3) != 0)
    //   return false;

    R_SignalHandlers = 0;

    structRstart Rst;
    memset(&Rst, 0, sizeof(Rst));

    R_setStartTime();
    R_DefParams(&Rst);

  //WIN32
    Rst.rhome = get_R_HOME();
    Rst.home = getRUser();
    Rst.CharacterMode = LinkDLL; //RGui
    Rst.ReadConsole = &local::readConsole;//NULL;
    Rst.WriteConsole = NULL;//writeOut;
    Rst.WriteConsoleEx = &local::writeOutEx;
    Rst.CallBack = local::myCallBack;
    Rst.ShowMessage = local::showMessage;
    Rst.YesNoCancel = yes_no_cancel;
    Rst.Busy = busy;

    Rst.R_Quiet = (Rboolean)bInteractive ? TRUE : FALSE;
    //Rst.RestoreAction = SA_RESTORE;
    Rst.SaveAction = SA_NOSAVE;
  //WIN32
    Rst.R_Interactive = (Rboolean)TRUE;// sets interactive() to eval to false 
    R_SetParams(&Rst);
    R_SizeFromEnv(&Rst);
    R_SetParams(&Rst);

    std::string rhome_bin(Rst.rhome);
#ifndef _WIN64
    rhome_bin += "\\bin\\i386";
#else
    rhome_bin += "\\bin\\x64";
#endif
    wchar_t cur_dir[MAX_PATH + 1] = {0};
    GetCurrentDirectory(MAX_PATH, cur_dir);
    SetCurrentDirectoryA(rhome_bin.c_str());

    //char* av[] = {"arcgis.rproxy", "--interactive"};//, "--no-readline", "--vanilla"};
    char* av[] = {"arcgis:rarcproxy", "--no-readline", "--vanilla"};
    //char* av[] = {"arcgis.rproxy", "--no-readline", "--no-save", "--no-restore"};
    //R_set_command_line_arguments(_countof(av), av);
    //bool ret = Rf_initEmbeddedR(_countof(av), av) == 1;
    R_set_command_line_arguments(_countof(av), av);
    //once = 1;
    HMODULE hGA = ::LoadLibrary(L"Rgraphapp.dll");
    typedef int (*fn_GA_initapp)(int, char **);
    fn_GA_initapp pGA = (fn_GA_initapp) GetProcAddress(hGA, "GA_initapp");
    (pGA)(0, 0);
    //GA_initapp(0,0);

    FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
    readconsolecfg();

    //setup_term_ui();
    setup_Rmainloop();
    //R_ReplDLLinit();
    if (cur_dir[0])
      SetCurrentDirectory(cur_dir);

    //if (ret)
    {
      ::PathRemoveFileSpecA(dllName);
      ::PathAppendA(dllName, "..\\..\\.."); //trim end: .\arcgisbinding\{i386,x64}\libs
      //tools::protect pt;
      Rf_defineVar(Rf_install(".arcgisbinding_inproc"), tools::newVal(true), R_GlobalEnv);
      std::string load_pkg("library(arcgisbinding,lib.loc='");
      load_pkg += dllName;
      load_pkg += "')";
      std::replace(load_pkg.begin(), load_pkg.end(),'\\','/');
      int x = evaluate_string(load_pkg.c_str());
      ATLASSERT(x);
      evaluate_string("options(browser = function(url) {.C('arc_browsehelp', url)})");
    }
    return true;
  }
};

  bool initInProcInterpreter(bool bInteractive = true)
  {
    static int once = 0;
    if (once) return once > 0 ? true : false;

    ::GetModuleFileName(::GetModuleHandle(L"AfCore.dll"), arcgis_path, MAX_PATH);
    ::PathRemoveFileSpec(arcgis_path);
    ::PathRemoveFileSpec(arcgis_path);

    g_main_TID = GetCurrentThreadId();
    once = -1;
    g_InProc = true;

    uintptr_t r_thread = ::_beginthread(&local::run_loop, 0, (LPVOID)&bInteractive);
    if (r_thread < 0)
      return false;
    int ret = 0;
    tchannel& channel = tchannel::singleton();
    while (true)
    {
      local::xwait(channel.from_thread.handle());
      tchannel::value p;
      channel.from_thread.pop(p, 0);
      if (p.empty())
        return false;

      if (local::process_message(p))
        break;
    }

    return true;
  }

  // this is maint thread
  int __cdecl do1line(const wchar_t* code)
  {
    tchannel::value _p(tchannel::R_CMD, 0);
    std::string statement = tools::toUtf8(code);
    if (!statement.empty())
    {
      //before process let check if wee need complete multiline code
      if (local::pre_eval2arc_cmd(statement.c_str()) == PARSE_INCOMPLETE)
        return PARSE_INCOMPLETE; //return to editor and add second propmt +>
      _p.data_int = 1;
    }
    else
      _p.data_int = 0;

    if (current_connect)
      current_connect->print_out(L"\n", 1);
    
    // main logic:
    // send command to R thread
    // wait for new prompt(R_PROMPT) - evaluation is done
    // if R script calls arc-bainding(FN_CALL) then process it here (main thread)
    // meanwhile process ouptut messages (R_TXT_OUT)
    tchannel& channel = tchannel::singleton();
    channel.to_thread.push(_p);
    while (true)
    {
      tchannel::value p;
      try
      {
        while(!channel.from_thread.pop(p, 0))
        {
          local::xwait(channel.from_thread.handle(), 0);
        }
      }catch(...){ return PARSE_ERROR; }
      if (local::process_message(p))
        return PARSE_OK;
    }
  }

}

bool isCancel()
{
  if (current_connect && current_connect->isCancel())
  {
    //UserBreak = 1;
    return true;
  }
  return false;
}
