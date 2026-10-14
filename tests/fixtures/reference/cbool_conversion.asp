<%
Option Explicit
Dim failures, checks, missing, result, number
failures = 0
checks = 0

Sub CheckValue(ByVal label, ByVal input, ByVal expected)
    Dim actual
    actual = CBool(input)
    checks = checks + 1
    If VarType(actual) <> 11 Or actual <> expected Then
        failures = failures + 1
        Response.Write "FAIL:" & label & ":" & actual & vbCrLf
    End If
End Sub

Sub CheckError(ByVal label, ByVal input, ByVal expected)
    Dim actual, number
    actual = "retained"
    On Error Resume Next
    Err.Clear
    actual = CBool(input)
    number = Err.Number
    Err.Clear
    On Error GoTo 0
    checks = checks + 1
    If number <> expected Or actual <> "retained" Then
        failures = failures + 1
        Response.Write "FAIL:" & label & ":error=" & number & vbCrLf
    End If
End Sub

Sub CoerceFlag(ByRef value)
    On Error Resume Next
    value = CBool(value)
    If Err.Number <> 0 Then
        Err.Clear
        If IsEmpty(value) Then
            value = True
        Else
            value = False
        End If
    End If
End Sub

CheckValue "false-text", "False", False
CheckValue "false-case", "fAlSe", False
CheckValue "true-case", "tRuE", True
CheckValue "spaces", "  False  ", False
CheckValue "zero-text", "0", False
CheckValue "negative-zero", "-0.000", False
CheckValue "positive-number", "+2.5", True
CheckValue "fraction", "0.000001", True
CheckValue "negative-number", "-1", True
CheckValue "exponent-zero", "0e10", False
CheckValue "exponent", "1e-10", True
CheckValue "hex-zero", "&H0", False
CheckValue "hex", "&HFFFF", True
CheckValue "octal", "&O10", True
CheckValue "long-zero", String(256, "0"), False
CheckValue "long-number", String(256, "0") & "1", True
CheckValue "empty", Empty, False
CheckValue "uninitialized", missing, False
CheckValue "boolean-false", False, False
CheckValue "boolean-true", True, True
CheckValue "integer-zero", 0, False
CheckValue "integer", -2, True
CheckValue "float-zero", 0.0, False
CheckValue "float", 0.25, True
CheckValue "date-zero", DateSerial(1899, 12, 30), False
CheckValue "date", DateSerial(2026, 9, 28), True
CheckError "null", Null, 94
CheckError "nothing", Nothing, 91
CheckError "array", Array(0, 1), 13
CheckError "empty-text", "", 13
CheckError "blank-text", "   ", 13
CheckError "invalid-text", "garbage", 13
CheckError "trailing-junk", "0garbage", 13
CheckError "nan", "NaN", 13
CheckError "infinity", "Infinity", 13
CheckError "c-hex", "0x1", 13
CheckError "partial-exponent", "1e", 13
CheckError "embedded-null", "0" & Chr(0) & "1", 13
CheckError "overflow", "1e9999", 6
On Error Resume Next
Err.Clear
result = CBool()
number = Err.Number
Err.Clear
On Error GoTo 0
checks = checks + 1
If number <> 450 Then
    failures = failures + 1
    Response.Write "FAIL:no-argument:error=" & number & vbCrLf
End If
On Error Resume Next
Err.Clear
result = CBool(0, 1)
number = Err.Number
Err.Clear
On Error GoTo 0
checks = checks + 1
If number <> 450 Then
    failures = failures + 1
    Response.Write "FAIL:extra-argument:error=" & number & vbCrLf
End If

' A search must retain its moderation state across repeated submissions.
CheckValue "normal-roundtrip", CStr(CBool("False")), False
CheckValue "pending-roundtrip", CStr(CBool("True")), True
result = ""
CoerceFlag result
CheckValue "missing-query-flag", result, False
result = "False"
CoerceFlag result
CheckValue "normal-query-flag", result, False
result = "True"
CoerceFlag result
CheckValue "pending-query-flag", result, True
Response.Write "checks=" & checks & ";failures=" & failures
%>
