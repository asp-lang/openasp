<%
Dim ok
ok = True
If Sgn(-2) <> -1 Then ok = False
If Sgn(0) <> 0 Then ok = False
If Sgn(2) <> 1 Then ok = False
If Abs(Atn(1) - 0.7853981633974483) > 0.000001 Then ok = False
If Abs(Sin(0)) > 0.000001 Then ok = False
If Abs(Cos(0) - 1) > 0.000001 Then ok = False
If Abs(Tan(0)) > 0.000001 Then ok = False
If Abs(Exp(0) - 1) > 0.000001 Then ok = False
If Abs(Log(1)) > 0.000001 Then ok = False
If ok Then
    Response.Write "ok"
Else
    Response.Write "failed"
End If
%>
