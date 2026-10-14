<%
Dim process
Dim result

Set process = Server.CreateObject("OpenASP.Process")
process.Timeout = 5000
process.MaxOutputBytes = 65536
Set result = process.Run("/bin/sh", "-c ""printf stdout; printf stderr >&2; exit 7""")
Response.Write CStr(result.ExitCode)
Response.Write "|" & result.StdOut
Response.Write "|" & result.StdErr
Response.Write "|" & CStr(result.TimedOut)
Set result = process.Run("/bin/sh", "-c ""sleep 2""", "", "", 50, 1024)
Response.Write "|" & CStr(result.ExitCode)
Response.Write "|" & CStr(result.TimedOut)
Set result = process.Start("/bin/sh", "-c ""sleep 5""")
Response.Write "|" & CStr(result.PID > 0)
Response.Write "|" & CStr(result.IsRunning)
Response.Write "|" & CStr(result.Terminate())
Response.Write "|" & CStr(result.Wait(5000))
Set result = process.Start("/bin/sh", "-c ""exit 3""")
Response.Write "|" & CStr(result.Wait(5000))

Dim childPid
Dim waitedPid
childPid = process.Fork()
If childPid = 0 Then
    Call process.Exit(23)
End If
waitedPid = process.WaitPid(-1, 5000)
Response.Write "|" & CStr(childPid > 0)
Response.Write "|" & CStr(waitedPid = childPid)
Response.Write "|" & CStr(process.ExitCode)
Response.Write "|" & CStr(process.TermSignal)
Response.Write "|" & CStr(process.LastPID = childPid)

childPid = process.Fork()
If childPid = 0 Then
    Do
    Loop
End If
Response.Write "|" & CStr(process.WaitPid(childPid, 0))
Response.Write "|" & CStr(process.Signal(childPid, 15))
waitedPid = process.WaitPid(-1, 5000)
Response.Write "|" & CStr(waitedPid = childPid)
Response.Write "|" & CStr(process.ExitCode)
Response.Write "|" & CStr(process.TermSignal)
%>
