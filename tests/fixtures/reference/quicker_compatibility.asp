<%
Response.ContentType = "text/plain"

Dim sample
sample = Array("a", "b")

Response.Write "date_parts=" & Year(DateSerial(2024, 2, 29)) & "-" & Month(DateSerial(2024, 2, 29)) & "-" & Day(DateSerial(2024, 2, 29)) & vbCrLf
Response.Write "date_add=" & Year(DateAdd("yyyy", 1, DateSerial(2024, 2, 29))) & "-" & Month(DateAdd("m", 1, DateSerial(2024, 1, 31))) & vbCrLf
Response.Write "date_diff=" & DateDiff("d", DateSerial(2024, 1, 1), DateSerial(2024, 1, 11)) & vbCrLf
Response.Write "date_part=" & DatePart("q", DateSerial(2024, 8, 1)) & vbCrLf
Response.Write "date_arithmetic=" & Day(DateSerial(2024, 1, 31) + 1) & vbCrLf
Response.Write "chars=" & Asc("A") & "," & AscB("Z") & "," & AscW(ChrW(20013)) & "," & ChrW(20013) & vbCrLf
Response.Write "bytes=" & LenB("abc") & "," & MidB("abcdef", 2, 3) & "," & InStrB(1, "abcdef", "cd") & vbCrLf
Response.Write "types=" & IsArray(sample) & "," & IsDate("2024-02-29") & "," & TypeName(sample) & "," & VarType("x") & vbCrLf
Response.Write "format=" & FormatNumber(12.345, 2) & "," & Space(2) & "x," & StrComp("ABC", "abc", 1) & vbCrLf
Response.Write "encode=" & Server.URLEncode("a b&" & ChrW(20013)) & "," & Server.HTMLEncode("<a&""'>") & vbCrLf

Dim dynamicValue
Execute "dynamicValue = 41 + 1"
Response.Write "execute=" & dynamicValue & ",eval=" & Eval("dynamicValue + 1") & vbCrLf

Session("compat") = "session"
Application("compat") = "application"
Session.Contents.Remove "compat"
Application.Contents.Remove "compat"
Response.Write "contents=" & IsEmpty(Session("compat")) & "," & IsEmpty(Application("compat")) & vbCrLf

On Error Resume Next
Err.Raise 123, "fixture", "expected"
Response.Write "err=" & Err.Number & "," & Err.Source & "," & Err.Description & vbCrLf
Err.Clear

Server.ScriptTimeout = 120
Response.ExpiresAbsolute = Now() - 1
Response.Cookies("compat") = "ok"
Response.Write "cookie_value_error=" & Err.Number & vbCrLf
Err.Clear
Response.Cookies("compat").Expires = DateAdd("d", 1, Now())
Response.Write "cookie_expires_error=" & Err.Number & vbCrLf
Response.Flush
%>
