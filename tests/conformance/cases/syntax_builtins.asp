<%
Dim values, text, payload, stamp
values = Array("a", "b", "c")
text = " AbC "
payload = ChrB(65) & ChrB(0) & ChrB(66)
stamp = DateSerial(2024, 2, 29)

Response.Write CStr(CBool(1)) & "|" & CStr(CInt(2.9)) & "|" & CStr(CLng(3.1)) & "|" & CStr(CDbl("4.5")) & "|"
Response.Write LCase(text) & "|" & UCase(Trim(text)) & "|" & Replace("a-b-a", "a", "x") & "|"
Response.Write Left("abcdef", 2) & "|" & Right("abcdef", 2) & "|" & Mid("abcdef", 2, 3) & "|"
Response.Write Len(values(0)) & "|" & Join(values, ":") & "|" & LBound(values) & "|" & UBound(values) & "|"
Response.Write InStr(1, "abcabc", "bc") & "|" & InStrRev("abcabc", "bc") & "|" & StrComp("a", "A", 1) & "|"
Response.Write Asc("A") & "|" & AscB(MidB(payload, 3, 1)) & "|" & LenB(payload) & "|"
Response.Write Hex(255) & "|" & CStr(Abs(-2)) & "|" & CStr(Sgn(-2)) & "|" & CStr(Int(-2.7)) & "|" & CStr(Fix(-2.7)) & "|"
Response.Write Year(stamp) & "-" & Month(stamp) & "-" & Day(stamp) & "|" & DateDiff("d", stamp, DateAdd("d", 2, stamp)) & "|"
Response.Write CStr(IsArray(values)) & "|" & CStr(IsDate(stamp)) & "|" & CStr(IsNumeric("12.5")) & "|" & TypeName(values) & "|" & VarType(values)
%>
