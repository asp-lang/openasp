<%
' Copyright (c) 2026 https://asp-lang.org
' SPDX-License-Identifier: MIT

Option Explicit

Const DefaultIterations = 100000000
Const ChecksumModulus = 1000003

Class MegaNode
    Public NextNode
    Public Payload
    Public Sequence
    Private storedValue

    Private Sub Class_Initialize()
        storedValue = 0
    End Sub

    Public Default Property Get Item(index)
        Item = storedValue + index
    End Property

    Public Default Property Let Item(index, value)
        storedValue = value - index
    End Property

    Public Property Get Value()
        Value = storedValue
    End Property

    Public Property Let Value(value)
        storedValue = value
    End Property

    Public Property Set Peer(value)
        Set NextNode = value
    End Property
End Class

Function MixValue(ByVal value, ByRef mutations)
    Dim mixed
    mixed = ((value * 17) + 31) Mod ChecksumModulus
    mixed = (mixed Xor (mixed \ 7)) And &H7FFFFFFF
    If (mixed Mod 2) = 0 Then
        mixed = (mixed \ 2) + 11
    Else
        mixed = ((mixed * 3) + 1) Mod ChecksumModulus
    End If
    mutations = mutations + 1
    MixValue = mixed Mod ChecksumModulus
End Function

Function Classify(ByVal value)
    Select Case value Mod 13
        Case 0, 2, 4, 6, 8, 10, 12
            Classify = 3
        Case 1, 5, 9
            Classify = 5
        Case Else
            Classify = 7
    End Select
End Function

Sub Mutate(ByRef value, ByVal delta)
    value = (value + delta) Mod ChecksumModulus
End Sub

Function ExerciseComponents(ByVal temporaryPath)
    Dim signature
    Dim dictionary, expression, matches, stream, document, root
    Dim json, parsed, crypto, fso, textFile
    Dim connection, command, parameter, parameters, rows, emptyRows

    signature = 0

    Set dictionary = Server.CreateObject("Scripting.Dictionary")
    dictionary.CompareMode = 1
    dictionary.Add "Alpha", 41
    dictionary("beta") = 1
    If dictionary.Exists("ALPHA") Then signature = signature + dictionary.Count
    dictionary.Remove "beta"

    Set expression = Server.CreateObject("VBScript.RegExp")
    expression.Pattern = "([a-z]+)([0-9]+)"
    expression.Global = True
    expression.IgnoreCase = True
    Set matches = expression.Execute("ab12 cd34")
    If expression.Test("xy99") Then signature = signature + matches.Count
    signature = signature + Len(expression.Replace("ab12", "$1"))

    Set stream = Server.CreateObject("ADODB.Stream")
    stream.Type = 2
    stream.Charset = "utf-8"
    stream.Open
    stream.WriteText "stream-data"
    stream.Position = 0
    signature = signature + Len(stream.ReadText)
    stream.Close

    Set document = Server.CreateObject("Microsoft.XMLDOM")
    document.async = False
    Call document.loadXML("<root><item id=""7"">value</item></root>")
    Set root = document.documentElement
    signature = signature + Len(root.nodeName) + document.childNodes.Count

    Set json = Server.CreateObject("OpenASP.JSON")
    Set parsed = json.Parse("{""name"":""OpenASP"",""items"":[1,true,null]}")
    signature = signature + Len(parsed("name")) + Len(json.Minify(" { ""x"" : 1 } "))
    If json.Validate("{""ok"":true}") Then signature = signature + 1

    Set crypto = Server.CreateObject("OpenASP.Crypto")
    signature = signature + Len(crypto.SHA256("abc"))
    signature = signature + Len(crypto.Base64Decode(crypto.Base64Encode("OpenASP")))
    If crypto.ConstantTimeEquals("same", "same") Then signature = signature + 1

    Set fso = Server.CreateObject("Scripting.FileSystemObject")
    Set textFile = fso.CreateTextFile(temporaryPath, True)
    textFile.WriteLine "openasp-service-gc"
    textFile.Close
    If fso.FileExists(temporaryPath) Then signature = signature + 1
    fso.DeleteFile temporaryPath

    Set connection = Server.CreateObject("ADODB.Connection")
    connection.Open "Provider=SQLite;Data Source=:memory:"
    Call connection.Execute("CREATE TABLE probe (id INTEGER PRIMARY KEY, name TEXT)")
    Call connection.Execute("INSERT INTO probe (id, name) VALUES (7, 'service')")
    Set command = Server.CreateObject("ADODB.Command")
    Set command.ActiveConnection = connection
    command.CommandText = "SELECT id, name FROM probe WHERE id = ?"
    Set parameter = command.CreateParameter("id", 3, 1, 0, 7)
    Set parameters = command.Parameters
    parameters.Append parameter
    Set rows = command.Execute()
    signature = signature + rows.Fields("id") + Len(rows.Fields("name")) + parameters.Count
    rows.Close
    connection.Close

    Set emptyRows = Server.CreateObject("ADODB.Recordset")
    signature = signature + UBound(emptyRows.GetRows, 1) + UBound(emptyRows.GetRows, 2) + 2
    ExerciseComponents = signature
End Function

Sub InitializeForkWorkers(ByRef workerIndex, ByRef processCount, ByRef childPids)
    Dim spawnIndex, forkResult, forkProcess

    processCount = CLng(Request.QueryString("processes"))
    If processCount < 1 Or processCount > 32 Then
        Response.Write "service-fork-controller-fail:processes"
        Response.End
    End If

    workerIndex = 0
    childPids = ""
    For spawnIndex = 1 To processCount - 1
        If workerIndex = 0 Then
            Set forkProcess = Server.CreateObject("OpenASP.Process")
            forkResult = forkProcess.Fork()
            If forkResult < 0 Then
                Response.Write "service-fork-controller-fail:fork:" & CStr(spawnIndex)
                Response.End
            ElseIf forkResult = 0 Then
                workerIndex = spawnIndex
            Else
                If Len(childPids) > 0 Then childPids = childPids & ","
                childPids = childPids & CStr(forkResult)
            End If
        End If
    Next
End Sub

Dim iterations, temporaryPath, parentIterations, forkMode, forkWorkerIndex, forkProcessCount, forkChildPids
Dim values(), matrix(1, 2), tokens
Dim i, j, phase, slotIndex, checksum, mutations, category, componentSignature
Dim textValue, parts, token, garbage, head, tail, dictionary, invalidValue, capturedError
Dim captured, http, ai, socket, redis, redisMulti, redisMultiClient, redisMultiCommand
Dim cache, cacheMulti, cacheMultiClient, cacheMultiCommand, websocket, process, shell

forkMode = LCase(CStr(Request.QueryString("role"))) = "fork"
forkWorkerIndex = 0
forkProcessCount = 1
forkChildPids = ""
If forkMode Then
    Call InitializeForkWorkers(forkWorkerIndex, forkProcessCount, forkChildPids)
End If

iterations = DefaultIterations
If IsNumeric(Request.QueryString("iterations")) Then iterations = CLng(Request.QueryString("iterations"))
parentIterations = Request.QueryString("parent_iterations")
If forkMode And forkWorkerIndex = 0 And Len(parentIterations) > 0 Then
    If IsNumeric(parentIterations) Then iterations = CLng(parentIterations)
End If
If iterations < 1 Then iterations = 1
If iterations > DefaultIterations Then iterations = DefaultIterations

temporaryPath = Request.QueryString("temp")
If Len(temporaryPath) = 0 Then temporaryPath = Server.MapPath("service_gc_megastress.tmp")
If forkMode Then temporaryPath = temporaryPath & ".worker-" & CStr(forkWorkerIndex)

componentSignature = ExerciseComponents(temporaryPath)
If componentSignature < 100 Then
    Response.Write "service-gc-fail:components:" & CStr(componentSignature)
    Response.End
End If

On Error Resume Next

Set http = Server.CreateObject("OpenASP.HttpClient")
http.Timeout = 20
http.MaxResponseBytes = 1024
http.SetHeader "X-OpenASP-Test", "service-gc"
captured = http.Get("http://127.0.0.1:1/unreachable")
If Err.Number <> 0 Then componentSignature = componentSignature + 1
Err.Clear
http.ClearHeaders

Set ai = Server.CreateObject("OpenASP.OpenAI")
ai.BaseURL = "http://127.0.0.1:1/v1"
ai.APIKey = "service-gc"
ai.Model = "probe"
ai.Timeout = 20
captured = ai.Responses("{""input"":""probe""}")
If Err.Number <> 0 Then componentSignature = componentSignature + 1
Err.Clear

Set socket = Server.CreateObject("OpenASP.Socket")
socket.Timeout = 20
socket.Open "tcp", "127.0.0.1", 1
If Err.Number <> 0 Then componentSignature = componentSignature + 1
Err.Clear
socket.Close

Set redis = Server.CreateObject("OpenASP.Redis")
redis.Timeout = 20
redis.Open "Host=127.0.0.1;Port=1;Database=0;Timeout=20"
If Err.Number <> 0 Then componentSignature = componentSignature + 1
Err.Clear
redis.Close

Set redisMulti = Server.CreateObject("OpenASP.RedisMulti")
Set redisMultiClient = redisMulti.CreateClient("Host=127.0.0.1;Port=1;Database=0;Timeout=20")
Set redisMultiCommand = redisMultiClient.CreateCommand("PING")
redisMulti.Timeout = 20
redisMulti.Execute
If redisMultiCommand.Completed Then componentSignature = componentSignature + 1

Set cacheMulti = Server.CreateObject("OpenASP.MemcacheMulti")
Set cacheMultiClient = cacheMulti.CreateClient("Host=127.0.0.1;Port=1;Timeout=20")
Set cacheMultiCommand = cacheMultiClient.CreateCommand("get", "probe")
cacheMulti.Timeout = 20
cacheMulti.Execute
If cacheMultiCommand.Completed Then componentSignature = componentSignature + 1

Set cache = Server.CreateObject("OpenASP.Memcache")
cache.Host = "127.0.0.1"
cache.Port = 1
cache.Timeout = 20
cache.Open
If Err.Number <> 0 Then componentSignature = componentSignature + 1
Err.Clear
cache.Close

Set websocket = Server.CreateObject("OpenASP.WebSocket")
websocket.Timeout = 20
websocket.Connect "ws://127.0.0.1:1/probe"
If Err.Number <> 0 Then componentSignature = componentSignature + 1
Err.Clear
websocket.Close

Set process = Server.CreateObject("OpenASP.Process")
If Err.Number = 70 Then componentSignature = componentSignature + 1
Err.Clear
Set shell = Server.CreateObject("Shell.Application")
If Err.Number = 70 Then componentSignature = componentSignature + 1
Err.Clear

On Error GoTo 0

ReDim values(31)
tokens = Array(1, 3, 5)
matrix(0, 0) = 2
matrix(0, 1) = 3
matrix(0, 2) = 5
matrix(1, 0) = 7
matrix(1, 1) = 11
matrix(1, 2) = 13

checksum = 97
mutations = 0
Set head = New MegaNode
Set tail = New MegaNode
head.Sequence = -1
tail.Sequence = -2
Set head.Peer = tail
Set tail.Peer = head

For i = 1 To iterations
    If i = 257 Then ReDim Preserve values(63)
    If i = 1025 Then ReDim Preserve values(127)

    slotIndex = i Mod (UBound(values) + 1)
    values(slotIndex) = MixValue(checksum + i + slotIndex, mutations)
    Call Mutate(values(slotIndex), Classify(i))
    checksum = (checksum + values(slotIndex) + matrix(i Mod 2, i Mod 3)) Mod ChecksumModulus

    j = 0
    While j < 2
        checksum = (checksum + j + (i Mod 17)) Mod ChecksumModulus
        j = j + 1
    Wend

    For Each token In tokens
        checksum = (checksum + token) Mod ChecksumModulus
    Next

    Do
        category = Classify(i + j)
        j = j - 1
    Loop Until j = 0

    Select Case category
        Case 3
            checksum = (checksum + 19) Mod ChecksumModulus
        Case 5
            checksum = (checksum + 23) Mod ChecksumModulus
        Case Else
            checksum = (checksum + 29) Mod ChecksumModulus
    End Select

    textValue = CStr(i) & ":" & CStr(slotIndex) & ":" & CStr(checksum)
    parts = Split(textValue, ":")
    If UBound(parts) <> 2 Or CLng(parts(0)) <> i Then
        Response.Write "service-gc-fail:string:" & CStr(i)
        Response.End
    End If

    Set garbage = New MegaNode
    With garbage
        .Sequence = i
        .Payload = String((i Mod 32) + 24, Chr(65 + (i Mod 26)))
        Set .NextNode = garbage
    End With
    garbage.Item(3) = checksum
    If garbage(3) <> checksum Then
        Response.Write "service-gc-fail:property:" & CStr(i)
        Response.End
    End If

    If (i Mod 113) = 0 Then
        tail.Payload = CStr(i) & ":" & garbage.Payload
        tail.Sequence = i
    End If

    If (i Mod 8192) = 0 Then
        Set dictionary = Server.CreateObject("Scripting.Dictionary")
        dictionary.Add "iteration", i
        checksum = (checksum + (dictionary("iteration") Mod 31)) Mod ChecksumModulus
        Set dictionary = Nothing
    End If

    For phase = 1 To 3
        values(slotIndex) = MixValue(checksum + i + phase, mutations)
        Call Mutate(values(slotIndex), phase + category)
        checksum = (checksum + values(slotIndex) + phase) Mod ChecksumModulus

        textValue = CStr(checksum) & ":" & CStr(i) & ":" & CStr(phase)
        parts = Split(textValue, ":")
        If UBound(parts) <> 2 Or CLng(parts(1)) <> i Then
            Response.Write "service-gc-fail:inner-string:" & CStr(i)
            Response.End
        End If

        Set garbage = New MegaNode
        garbage.Sequence = i + phase
        garbage.Payload = Replace(String((i Mod 24) + 16, Chr(70 + phase)), "Z", "Y")
        garbage.Value = checksum
        Set garbage.NextNode = garbage
        If garbage.Value <> checksum Then
            Response.Write "service-gc-fail:inner-property:" & CStr(i)
            Response.End
        End If
    Next
Next

If mutations <> (iterations * 4) Then
    Response.Write "service-gc-fail:byref:" & CStr(mutations)
    Response.End
End If

On Error Resume Next
invalidValue = 1 / 0
capturedError = Err.Number
Err.Clear
On Error GoTo 0
If capturedError = 0 Then
    Response.Write "service-gc-fail:error-state"
    Response.End
End If

If head.NextNode.Sequence <> tail.Sequence Then
    Response.Write "service-gc-fail:object-graph"
    Response.End
End If

Erase values
If forkMode Then
    Response.Write "service-fork-gc-ok|" & CStr(forkWorkerIndex) & "|" & _
        CStr(forkProcessCount) & "|" & CStr(iterations) & "|" & CStr(checksum) & "|" & _
        CStr(mutations) & "|" & CStr(componentSignature) & "|" & CStr(tail.Sequence) & "|" & _
        forkChildPids & vbCrLf
Else
    Response.Write "service-gc-ok|" & CStr(iterations) & "|" & CStr(checksum) & "|" & _
        CStr(mutations) & "|" & CStr(componentSignature) & "|" & CStr(tail.Sequence)
End If
%>
