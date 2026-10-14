<%
Option Explicit
Dim s
For Each s In Array("2026-9","2026-09","2026/9"," 2026-09 ","2024-2","2026-12")
    Response.Write IsDate(s) & ":" & Year(CDate(s)) & "-" & Month(CDate(s)) & "-" & Day(CDate(s)) & "|"
Next
For Each s In Array("2026-0","2026-13","2026-09-","2026-09x","2026/9x","2026-09-31","2026-02-29")
    Response.Write IsDate(s) & "|"
Next
Response.Write IsDate("2024-02-29") & "|"
Response.Write Hour(CDate("2026-09-28 12:34:56")) & ":" & Minute(CDate("2026-09-28 12:34:56")) & ":" & Second(CDate("2026-09-28 12:34:56")) & "|"
Response.Write Year(CDate("9/28/2026")) & "-" & Month(CDate("9/28/2026")) & "-" & Day(CDate("9/28/2026"))
%>
