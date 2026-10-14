<%
' Copyright (c) 2026 https://asp-lang.org
' SPDX-License-Identifier: MIT

Option Explicit

Const WorkerCount = 8
Const SampleWidth = 8
Const ChecksumModulus = 1000003
Const MaximumRequestBytes = 65536

Function ReadHttpRequest(client, ByRef buffered)
    Dim marker, chunk, headerText, headerLines, requestParts
    Dim line, index, contentLength, totalLength, connectionValue, method, path, body, keepAlive

    marker = InStr(1, buffered, vbCrLf & vbCrLf, vbBinaryCompare)
    Do While marker = 0
        chunk = client.Read(MaximumRequestBytes)
        If LenB(chunk) = 0 Then
            ReadHttpRequest = Array(False, "", "", "", False)
            Exit Function
        End If
        buffered = buffered & chunk
        If LenB(buffered) > MaximumRequestBytes Then
            ReadHttpRequest = Array(False, "", "", "", False)
            Exit Function
        End If
        marker = InStr(1, buffered, vbCrLf & vbCrLf, vbBinaryCompare)
    Loop

    headerText = Left(buffered, marker - 1)
    headerLines = Split(headerText, vbCrLf)
    requestParts = Split(headerLines(0), " ")
    If UBound(requestParts) < 2 Then
        ReadHttpRequest = Array(False, "", "", "", False)
        Exit Function
    End If

    method = requestParts(0)
    path = requestParts(1)
    contentLength = 0
    connectionValue = ""
    For index = 1 To UBound(headerLines)
        line = headerLines(index)
        If LCase(Left(line, 15)) = "content-length:" Then
            contentLength = CLng(Trim(Mid(line, 16)))
        ElseIf LCase(Left(line, 11)) = "connection:" Then
            connectionValue = LCase(Trim(Mid(line, 12)))
        End If
    Next

    If contentLength < 0 Or contentLength > MaximumRequestBytes Then
        ReadHttpRequest = Array(False, "", "", "", False)
        Exit Function
    End If
    totalLength = marker + 3 + contentLength
    Do While LenB(buffered) < totalLength
        chunk = client.Read(totalLength - LenB(buffered))
        If LenB(chunk) = 0 Then
            ReadHttpRequest = Array(False, "", "", "", False)
            Exit Function
        End If
        buffered = buffered & chunk
    Loop

    body = Mid(buffered, marker + 4, contentLength)
    If LenB(buffered) = totalLength Then
        buffered = ""
    Else
        buffered = Mid(buffered, totalLength + 1)
    End If
    keepAlive = connectionValue <> "close"
    ReadHttpRequest = Array(True, method, path, body, keepAlive)
End Function

Function PayloadChecksum(payload, requestId, batchCount, workerIndex, endpointCode)
    Dim index, logicalIndex, checksum
    checksum = (requestId * 17 + batchCount * 31 + workerIndex * 13 + endpointCode * 7) Mod ChecksumModulus
    For index = 1 To Len(payload)
        checksum = (checksum * 33 + Asc(Mid(payload, index, 1)) + index) Mod ChecksumModulus
    Next
    For logicalIndex = 1 To batchCount
        checksum = (checksum * 17 + logicalIndex + requestId + workerIndex + endpointCode) Mod ChecksumModulus
        checksum = (checksum Xor (checksum \ 13)) And &H7FFFFFFF
        checksum = checksum Mod ChecksumModulus
    Next
    PayloadChecksum = checksum
End Function

Sub ForkWorkers(ByRef workerIndex, ByRef childPids)
    Dim spawnIndex, forkResult, process
    workerIndex = 0
    childPids = ""
    For spawnIndex = 1 To WorkerCount - 1
        If workerIndex = 0 Then
            Set process = Server.CreateObject("OpenASP.Process")
            forkResult = process.Fork()
            If forkResult < 0 Then
                Response.Write "fork-http-json-fail|fork|" & CStr(spawnIndex)
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

Function EndpointCode(path)
    Select Case path
        Case "/kv/batch"
            EndpointCode = 1
        Case "/list/window"
            EndpointCode = 2
        Case "/zset/rank"
            EndpointCode = 3
        Case "/hash/profile"
            EndpointCode = 4
        Case "/mixed/transaction"
            EndpointCode = 5
        Case Else
            EndpointCode = 0
    End Select
End Function

Function ExerciseEndpoint(redis, context, path, requestData, json)
    Dim code, checksum, index, keySuffix, value, progress, prefix, workerIndex
    Dim requestId, batchCount, payload, outputItems, replyItems, responseData
    Dim kvArgs(), keys(), listValues(), zsetArgs(), hashArgs(), fields()
    Dim kvReply, listReply, zsetReply, hashReply
    Dim kvKey, listKey, zsetKey, hashKey

    prefix = CStr(context("prefix"))
    workerIndex = CLng(context("worker"))
    requestId = CLng(requestData("request_id"))
    batchCount = CLng(requestData("batch"))
    payload = CStr(requestData("payload"))
    code = EndpointCode(path)
    checksum = PayloadChecksum(payload, requestId, batchCount, workerIndex, code)
    kvKey = prefix & ":kv:"
    listKey = prefix & ":list:" & CStr(workerIndex)
    zsetKey = prefix & ":zset:" & CStr(workerIndex)
    hashKey = prefix & ":hash:" & CStr(workerIndex)

    ReDim kvArgs(SampleWidth * 2 - 1)
    ReDim keys(SampleWidth - 1)
    ReDim listValues(SampleWidth - 1)
    ReDim zsetArgs(SampleWidth * 2 - 1)
    ReDim hashArgs(SampleWidth * 2 - 1)
    ReDim fields(SampleWidth - 1)
    For index = 0 To SampleWidth - 1
        keySuffix = CStr(workerIndex) & ":" & CStr(index)
        value = CStr(requestId) & ":" & CStr(checksum) & ":" & CStr(index) & ":" & _
                Left(payload, 96) & ":" & String(32, Chr(65 + ((requestId + index) Mod 26)))
        keys(index) = kvKey & keySuffix
        kvArgs(index * 2) = keys(index)
        kvArgs(index * 2 + 1) = value
        listValues(index) = value
        zsetArgs(index * 2) = requestId * SampleWidth + index
        zsetArgs(index * 2 + 1) = keySuffix & ":" & CStr(checksum)
        fields(index) = "field:" & CStr(index)
        hashArgs(index * 2) = fields(index)
        hashArgs(index * 2 + 1) = value
    Next

    replyItems = 0
    Select Case code
        Case 1
            Call redis.Execute("MSET", kvArgs)
            kvReply = redis.Execute("MGET", keys)
            outputItems = kvReply
            If IsArray(kvReply) Then replyItems = UBound(kvReply) + 1
        Case 2
            Call redis.Execute("LPUSH", listKey, listValues)
            Call redis.Execute("LTRIM", listKey, 0, 255)
            listReply = redis.Execute("LRANGE", listKey, 0, SampleWidth - 1)
            outputItems = listReply
            If IsArray(listReply) Then replyItems = UBound(listReply) + 1
        Case 3
            Call redis.Execute("ZADD", zsetKey, zsetArgs)
            zsetReply = redis.Execute("ZRANGE", zsetKey, 0, SampleWidth - 1, "WITHSCORES")
            outputItems = zsetReply
            If IsArray(zsetReply) Then replyItems = UBound(zsetReply) + 1
        Case 4
            Call redis.Execute("HSET", hashKey, hashArgs)
            hashReply = redis.Execute("HMGET", hashKey, fields)
            outputItems = hashReply
            If IsArray(hashReply) Then replyItems = UBound(hashReply) + 1
        Case 5
            Call redis.Execute("MSET", kvArgs)
            kvReply = redis.Execute("MGET", keys)
            Call redis.Execute("LPUSH", listKey, listValues)
            Call redis.Execute("LTRIM", listKey, 0, 255)
            listReply = redis.Execute("LRANGE", listKey, 0, SampleWidth - 1)
            Call redis.Execute("ZADD", zsetKey, zsetArgs)
            zsetReply = redis.Execute("ZRANGE", zsetKey, 0, SampleWidth - 1, "WITHSCORES")
            Call redis.Execute("HSET", hashKey, hashArgs)
            hashReply = redis.Execute("HMGET", hashKey, fields)
            outputItems = hashReply
            replyItems = (UBound(kvReply) + 1) + (UBound(listReply) + 1) + _
                         (UBound(zsetReply) + 1) + (UBound(hashReply) + 1)
    End Select

    progress = redis.Execute("INCRBY", prefix & ":progress", batchCount)
    If (progress Mod 1000000) = 0 Then
        Call redis.Execute("HSET", prefix & ":milestones", CStr(progress), _
                           CStr(workerIndex) & ":" & CStr(requestId) & ":" & CStr(checksum))
    End If
    Set responseData = Server.CreateObject("Scripting.Dictionary")
    responseData.Add "ok", True
    responseData.Add "request_id", requestId
    responseData.Add "worker", workerIndex
    responseData.Add "checksum", checksum
    responseData.Add "progress", progress
    responseData.Add "endpoint_code", code
    responseData.Add "reply_items", replyItems
    responseData.Add "items", outputItems
    ExerciseEndpoint = json.Stringify(responseData)
End Function

Function HttpPacket(statusText, responseBody, closeConnection)
    Dim connectionHeader
    If closeConnection Then
        connectionHeader = "close"
    Else
        connectionHeader = "keep-alive"
    End If
    HttpPacket = "HTTP/1.1 " & statusText & vbCrLf & _
                 "Content-Type: application/json" & vbCrLf & _
                 "Content-Length: " & CStr(LenB(responseBody)) & vbCrLf & _
                 "Connection: " & connectionHeader & vbCrLf & vbCrLf & responseBody
End Function

Dim listener, client, redis, json, requestData, context, requestInfo
Dim workerIndex, childPids, port, prefix, method, path, body, buffered, batchCount
Dim requestsHandled, logicalHandled, responseBody, responsePacket
Dim capturedError, stopWorker, connectionDone, keepAlive

port = CLng(Request.QueryString("port"))
prefix = CStr(Request.QueryString("prefix"))
If port < 1 Or Len(prefix) = 0 Then
    Response.Write "fork-http-json-fail|configuration"
    Response.End
End If

Set listener = Server.CreateObject("OpenASP.Socket")
listener.Protocol = "tcp"
listener.Timeout = 1000
Call listener.Listen("127.0.0.1", port, 256)
Call ForkWorkers(workerIndex, childPids)

Set redis = Server.CreateObject("OpenASP.Redis")
Call redis.Open("Host=127.0.0.1;Port=6379;Database=15;Timeout=5000")
Call redis.Execute("HSET", prefix & ":workers", CStr(workerIndex), CStr(workerIndex))
Set json = Server.CreateObject("OpenASP.JSON")
Set context = Server.CreateObject("Scripting.Dictionary")
context.Add "prefix", prefix
context.Add "worker", workerIndex

requestsHandled = 0
logicalHandled = 0
stopWorker = False
Do While Not stopWorker
    Set client = Nothing
    On Error Resume Next
    Set client = listener.Accept(1000)
    capturedError = Err.Number
    Err.Clear
    On Error GoTo 0
    If capturedError = 0 Then
        buffered = ""
        connectionDone = False
        Do While Not connectionDone And Not stopWorker
            requestInfo = ReadHttpRequest(client, buffered)
            If Not requestInfo(0) Then
                connectionDone = True
            Else
                method = requestInfo(1)
                path = requestInfo(2)
                body = requestInfo(3)
                keepAlive = requestInfo(4)
            End If
            If Not connectionDone And method <> "POST" Then
                responseBody = "{""ok"":false,""error"":""method""}"
                Call client.Write(HttpPacket("405 Method Not Allowed", responseBody, True))
                connectionDone = True
            ElseIf Not connectionDone And path = "/admin/stop" Then
                responseBody = "{""ok"":true,""stopped_worker"":" & CStr(workerIndex) & "}"
                Call client.Write(HttpPacket("200 OK", responseBody, True))
                stopWorker = True
                connectionDone = True
            ElseIf Not connectionDone Then
                Set requestData = json.Parse(body)
                batchCount = CLng(requestData("batch"))
                responseBody = ExerciseEndpoint(redis, context, path, requestData, json)
                responsePacket = HttpPacket("200 OK", responseBody, Not keepAlive)
                Call client.Write(responsePacket)
                requestsHandled = requestsHandled + 1
                logicalHandled = logicalHandled + batchCount
                If Not keepAlive Then connectionDone = True
            End If
        Loop
        client.Close
    End If
Loop

redis.Close
listener.Close
Response.Write "fork-http-json-worker-ok|" & CStr(workerIndex) & "|" & CStr(requestsHandled) & "|" & _
               CStr(logicalHandled) & "|" & childPids & vbCrLf
%>
