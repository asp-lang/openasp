<%
Option Explicit
On Error Resume Next

Function NormalizeLong(ByRef source)
    source = CLng(source) + 100
    NormalizeLong = True
End Function

Function NormalizeLongWithObject(ByRef source)
    On Error Resume Next
    source = CLng(source) + 100
    NormalizeLongWithObject = True
End Function

Dim emptyHook
emptyHook = ""

Function EmptyExecuteFilter(ByRef scalarField, ByRef objectValue)
    If emptyHook = "" Then Exit Function
    Execute emptyHook
End Function

Function EmptyExecuteFilter13(ByRef v1, ByRef v2, ByRef v3, ByRef v4, ByRef v5, ByRef v6, ByRef v7, ByRef v8, ByRef v9, ByRef v10, ByRef v11, ByRef v12, ByRef objectValue)
    If emptyHook = "" Then Exit Function
    Execute emptyHook
End Function

Class ProbeHolder
    Public ID
    Public V2
    Public V3
    Public V4
    Public V5
    Public V6
    Public V7
    Public V8
    Public V9
    Public V10
    Public V11
    Public V12

    Public Function NormalizeField()
        Call NormalizeLong(ID)
        NormalizeField = ID
    End Function

    Public Function NormalizeLocal()
        Dim localValue
        localValue = ID
        Call NormalizeLong(localValue)
        NormalizeLocal = localValue
    End Function

    Public Function NormalizeFieldWithObject()
        Call NormalizeLongWithObject(ID)
        NormalizeFieldWithObject = ID
    End Function

    Public Function NormalizeAfterMethodObject()
        On Error Resume Next
        Call NormalizeLong(ID)
        NormalizeAfterMethodObject = ID
    End Function

    Public Function RunEmptyFilter(objectValue)
        On Error Resume Next
        Call EmptyExecuteFilter(ID, objectValue)
        RunEmptyFilter = Err.Number
    End Function

    Public Function RunEmptyFilter13(objectValue)
        On Error Resume Next
        Call EmptyExecuteFilter13(ID, V2, V3, V4, V5, V6, V7, V8, V9, V10, V11, V12, objectValue)
        RunEmptyFilter13 = Err.Number
    End Function
End Class

Dim localValue
localValue = "11"
Call NormalizeLong(localValue)
Response.Write "global=" & localValue & ";"

Dim holder
Set holder = New ProbeHolder
holder.ID = "12"
Response.Write "method-local=" & holder.NormalizeLocal() & ";"

holder.ID = "13"
Response.Write "field=" & holder.NormalizeField() & ";"

holder.ID = "14"
Call NormalizeLong(holder.ID)
Response.Write "explicit=" & holder.ID & ";"

holder.ID = "15"
Response.Write "object-field=" & holder.NormalizeFieldWithObject() & ";"

holder.ID = "16"
Response.Write "method-object=" & holder.NormalizeAfterMethodObject() & ";"

Dim secondHolder
Set secondHolder = New ProbeHolder
holder.ID = "17"
Response.Write "empty-filter-err=" & holder.RunEmptyFilter(secondHolder) & ";field=" & holder.ID & ";"

holder.ID = "18"
Response.Write "empty-filter-13-err=" & holder.RunEmptyFilter13(secondHolder) & ";field13=" & holder.ID & ";err=" & Err.Number
%>
