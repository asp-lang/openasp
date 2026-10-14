<%
Dim process
Dim pid

Set process = Server.CreateObject("OpenASP.Process")
Set process = process.Start("/bin/sh", "-c ""sleep 5""")
pid = process.PID
Response.Write CStr(pid > 0) & "|" & CStr(process.IsRunning) & "|"
Response.Write CStr(process.Terminate()) & "|" & CStr(process.Wait(5000)) & "|"
Response.Write CStr(process.LastPID = pid) & "|" & CStr(process.TermSignal)
%>
