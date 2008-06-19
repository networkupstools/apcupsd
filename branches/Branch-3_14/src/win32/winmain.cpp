// This file has been adapted to the Win32 version of Apcupsd
// by Kern E. Sibbald.  Many thanks to ATT and James Weatherall,
// the original author, for providing an excellent template.
//
// Rewrite/Refactoring by Adam Kropelin
//
// Copyright (2007) Adam D. Kropelin
// Copyright (2000-2006) Kern E. Sibbald
//     20 July 2000

// System Headers
#include <unistd.h>
#include <windows.h>
#include <lmcons.h>
#include <ctype.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include "defines.h"
#include "accctrl.h"
#include "aclapi.h"

// Apcupsd UNIX main entrypoint
extern int ApcupsdMain(int argc, char **argv);

// Custom headers
#include "winups.h"
#include "winservice.h"
#include "compat.h"

// Standard command-line flag definitions
#define ApcupsdRunService        "/service"
#define ApcupsdRunAsUserApp      "/run"
#define ApcupsdInstallService    "/install"
#define ApcupsdRemoveService     "/remove"
#define ApcupsdKillRunningCopy   "/kill"
#define ApcupsdShowHelp          "/help"
#define ApcupsdQuiet             "/quiet"

// Usage string
static const char *ApcupsdUsageText =
   "apcupsd [/quiet] [/run] [/kill] [/install] [/remove] [/help]\n";

// Application instance
static HINSTANCE hAppInstance;

// Command line argument storage
#define MAX_COMMAND_ARGS 100
static char *command_args[MAX_COMMAND_ARGS] = { "apcupsd", NULL };
static int num_command_args = 1;
static char *winargs[MAX_COMMAND_ARGS];
static int num_winargs = 0;

// WinMain parses the command line and either calls the main App
// routine or, under NT, the main service routine.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   PSTR CmdLine, int iCmdShow)
{
   bool quiet = false;

   InitWinAPIWrapper();
   WSA_Init();

   // Save the application instance and main thread id
   hAppInstance = hInstance;

   /* Build Unix style argc *argv[] */

   /* Don't NULL command_args[0] !!! */
   for (int i = 1; i < MAX_COMMAND_ARGS; i++)
      command_args[i] = NULL;

   // Split command line to windows and non-windows arguments
   char *arg;
   char *szCmdLine = CmdLine;
   while ((arg = GetArg(&szCmdLine))) {
      // Save the argument in appropriate list
      if (*arg != '/' && num_command_args < MAX_COMMAND_ARGS)
         command_args[num_command_args++] = arg;
      else if (num_winargs < MAX_COMMAND_ARGS)
         winargs[num_winargs++] = arg;
   }

   // Default Windows argument
   if (num_winargs == 0)
      winargs[num_winargs++] = ApcupsdRunAsUserApp;

   // Act on Windows arguments...
   for (int i = 0; i < num_winargs; i++) {
      // /service
      if (strcasecmp(winargs[i], ApcupsdRunService) == 0) {
         // Run Apcupsd as a service
         return upsService::ApcupsdServiceMain();
      }
      // /run  (this is the default if no command line arguments)
      if (strcasecmp(winargs[i], ApcupsdRunAsUserApp) == 0) {
         // Apcupsd is being run as a user-level program
         return ApcupsdAppMain(0);
      }
      // /install
      if (strcasecmp(winargs[i], ApcupsdInstallService) == 0) {
         // Install Apcupsd as a service
         return upsService::InstallService(quiet);
      }
      // /remove
      if (strcasecmp(winargs[i], ApcupsdRemoveService) == 0) {
         // Remove the Apcupsd service
         return upsService::RemoveService(quiet);
      }
      // /kill
      if (strcasecmp(winargs[i], ApcupsdKillRunningCopy) == 0) {
         // Kill any already running copy of Apcupsd
         ApcupsdTerminate();
         return 0;
      }
      // /quiet
      if (strcasecmp(winargs[i], ApcupsdQuiet) == 0) {
         // Set quiet flag and go on to next argument
         quiet = true;
         continue;
      }
      // /help
      if (strcasecmp(winargs[i], ApcupsdShowHelp) == 0) {
         MessageBox(NULL, ApcupsdUsageText, "Apcupsd Usage",
                    MB_OK | MB_ICONINFORMATION);
         return 0;
      }

      // Unknown option: Show the usage dialog
      MessageBox(NULL, winargs[i], "Bad Command Line Options", MB_OK);
      MessageBox(NULL, ApcupsdUsageText, "Apcupsd Usage",
                 MB_OK | MB_ICONINFORMATION);
      return 1;
   }
}

// Callback for processing Windows messages
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
   switch (uMsg)
   {
   // Clean exit requested
   case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

   // Everything else uses default handling
   default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
   }
}

static void PostToApcupsd(UINT message, WPARAM wParam, LPARAM lParam)
{
  // Locate the hidden Apcupsd window
  HWND hservwnd = FindWindowEx(NULL, NULL, APCUPSD_WINDOW_CLASS, APCUPSD_WINDOW_NAME);
  if (hservwnd == NULL)
     return;

  // Post the message to Apcupsd
  PostMessage(hservwnd, message, wParam, lParam);
}

void ApcupsdTerminate()
{
   // Old versions of apcupsd and modern versions running on WinNT and
   // earlier need to receive a window message.
   PostToApcupsd(WM_CLOSE, 0, 0);

   // New apcupsd on Win2K and above listen for an event to be signaled.
   // This allows stopping apcupsd instances running under LocalSystem
   // and those started on other desktops.
   if (g_os_version >= WINDOWS_2000)
   {
      HANDLE evt = OpenEvent(EVENT_MODIFY_STATE, FALSE, APCUPSD_STOP_EVENT_NAME);
      if (evt != NULL)
      {
         SetEvent(evt);
         CloseHandle(evt);
      }
   }
}

// Called as a thread from ApcupsdAppMain()
// Here we invoke apcupsd UNIX main loop
void *ApcupsdMain(LPVOID lpwThreadParam)
{
   pthread_detach(pthread_self());

   // Call the "real" apcupsd
   ApcupsdMain(num_command_args, command_args);

   // In case apcupsd returns, terminate application
   ApcupsdTerminate();
}

// Add the requested access to the given kernel object handle
static bool GrantAccess(HANDLE h, ACCESS_MASK access, TRUSTEE_TYPE type, LPTSTR name)
{
   DWORD rc;

   // Obtain current DACL from object
   ACL *dacl;
   SECURITY_DESCRIPTOR *sd;
   rc = GetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, 
                        NULL, NULL, &dacl, NULL, &sd);
   if (rc != ERROR_SUCCESS)
      return false;

   // Add requested access to DACL
   EXPLICIT_ACCESS ea;
   ea.grfAccessPermissions = access;
   ea.grfAccessMode = GRANT_ACCESS;
   ea.grfInheritance = NO_INHERITANCE;
   ea.Trustee.pMultipleTrustee = FALSE;
   ea.Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
   ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
   ea.Trustee.TrusteeType = type;
   ea.Trustee.ptstrName = name;
   ACL *newdacl;
   rc = SetEntriesInAcl(1, &ea, dacl, &newdacl);
   if (rc != ERROR_SUCCESS) {
      LocalFree(sd);
      return false;
   }

   // Set new DACL on object
   rc = SetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, 
                        NULL, NULL, newdacl, NULL);

   // Done with structs
   LocalFree(newdacl);
   LocalFree(sd);

   return rc == ERROR_SUCCESS;
}

// Wait for exit signal on WinNT and below. This code creates a hidden
// window and waits for a WM_CLOSE window message to be delivered. We only
// use this on ancient platforms that do not support named events.
static void WaitForExit()
{
   // Dummy window class
   WNDCLASSEX wndclass;
   wndclass.cbSize = sizeof(wndclass);
   wndclass.style = 0;
   wndclass.lpfnWndProc = WindowProc;
   wndclass.cbClsExtra = 0;
   wndclass.cbWndExtra = 0;
   wndclass.hInstance = hAppInstance;
   wndclass.hIcon = NULL;
   wndclass.hCursor = NULL;
   wndclass.hbrBackground = NULL;
   wndclass.lpszMenuName = NULL;
   wndclass.lpszClassName = APCUPSD_WINDOW_CLASS;
   wndclass.hIconSm = NULL;
   if (RegisterClassEx(&wndclass) == 0)
      return;

   // Create dummy window so we can receive Windows messages
   HWND hwnd = CreateWindow(APCUPSD_WINDOW_CLASS,  // class
                            APCUPSD_WINDOW_NAME,   // name/title
                            0,                     // style
                            0,                     // X pos
                            0,                     // Y pos
                            0,                     // width
                            0,                     // height
                            NULL,                  // parent
                            NULL,                  // menu
                            hAppInstance,          // app instance
                            NULL );                // create param
   if (hwnd == NULL)
      return;

   // Now enter the Windows message handling loop until told to quit
   MSG msg;
   if (g_os_version < WINDOWS_2000)
   {
      // On older systems, all we can do is wait for a window message
      // and process it. We'll automatically exit if we get WM_QUIT.
      while (GetMessage(&msg, NULL, 0, 0))
      {
         TranslateMessage(&msg);
         DispatchMessage(&msg);
      }
   }
   else
   {
      // On newer platforms, we wait for a WM_QUIT window message as above,
      // or for a named event to be signaled. This code creates an event,
      // grants access to all users in the Adminstrators group, then waits
      // for the event to be signaled. We prefer this method on modern 
      // platforms since it allows terminating an apcupsd running in another
      // session. We still do the window message thing, too, for backwards
      // compatibility.

      // Create event to signal shutdown
      HANDLE hevt = CreateEvent(NULL, TRUE, FALSE, APCUPSD_STOP_EVENT_NAME);

      // Add EVENT_MODIFY_STATE permission for Administrators group
      GrantAccess(hevt, EVENT_MODIFY_STATE, TRUSTEE_IS_GROUP, "Administrators");

      DWORD rc;
      do
      {
         // Wait for a message to arrive or for the stop event to be signaled
         rc = MsgWaitForMultipleObjects(1, &hevt, FALSE, INFINITE, QS_ALLEVENTS);

         // Stop event was signaled or major error: Time to exit
         if (rc != WAIT_OBJECT_0+1)
            break;

         // Message must be ready: fetch and translate it
         while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
         {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
         }
      }
      while(LOWORD(msg.message) != WM_QUIT);

      CloseHandle(hevt);
   }

   DestroyWindow(hwnd);
}

// This is the main routine for Apcupsd. It spawns a thread to run the
// UNIX apcupsd back end and waits to be told to exit.
int ApcupsdAppMain(int service)
{
   // Set this process to be the last application to be shut down.
   SetProcessShutdownParameters(0x100, 0);

   // Check to see if we're already running
   HANDLE sem = CreateSemaphore(NULL, 0, 1, "apcupsd");
   if (sem == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
      MessageBox(NULL, "Another instance of Apcupsd is already running", 
                 "Apcupsd Error", MB_OK);
      return 0;
   }

   // Create a thread on which to run apcupsd UNIX main loop
   pthread_t tid;
   pthread_create(&tid, NULL, ApcupsdMain, (void *)GetCurrentThreadId());

   // Wait for exit request.
   WaitForExit();

   pthread_kill(tid, SIGTERM);
   return 0;
}
