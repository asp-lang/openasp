<%
' Copyright (c) 2026 https://asp-lang.org
' SPDX-License-Identifier: MIT

Option Explicit

Const WorkerCount = 8
Const ProtocolVersion = 1
Const OpcodeWork = 1
Const OpcodeStop = 2
Const ResponseOpcode = 129
Const RequestHeaderLength = 18
Const ChecksumModulus = 1000003
Const SampleWidth = 8

Function ByteAt(value, position)
    ByteAt = AscB(MidB(value, position, 1))
End Function

Function DecodeUInt32(value, position)
    DecodeUInt32 = CLng(ByteAt(value, position)) * 16777216 + _
                   CLng(ByteAt(value, position + 1)) * 65536 + _
                   CLng(ByteAt(value, position + 2)) * 256 + _
                   CLng(ByteAt(value, position + 3))
End Function

Function EncodeUInt32(value)
    Dim remaining
    remaining = CDbl(value)
    EncodeUInt32 = ChrB((remaining \ 16777216) Mod 256) & _
                   ChrB((remaining \ 65536) Mod 256) & _
                   ChrB((remaining \ 256) Mod 256) & _
                   ChrB(remaining Mod 256)
End Function

Function ReadExact(client, required)
    Dim output, chunk
    output = ""
    Do While LenB(output) < required
        chunk = client.Read(required - LenB(output))
        If LenB(chunk) = 0 Then Exit Do
        output = output & chunk
    Loop
    ReadExact = output
End Function

Function PayloadChecksum(payload, requestId, batchCount)
    Dim index, checksum
    checksum = (requestId * 17 + batchCount * 31) Mod ChecksumModulus
    For index = 1 To LenB(payload)
        checksum = (checksum * 33 + ByteAt(payload, index) + index) Mod ChecksumModulus
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
                Response.Write "fork-rpc-fail|fork|" & CStr(spawnIndex)
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

Function ExerciseRedis(redis, prefix, workerIndex, requestId, batchCount, payload, ByRef replyItems)
    Dim checksum, index, logicalIndex, keySuffix, value
    Dim kvArgs(), keys(), listValues(), zsetArgs(), hashArgs(), fields()
    Dim kvReply, listReply, zsetReply, hashReply, progress
    Dim kvKey, listKey, zsetKey, hashKey

    checksum = PayloadChecksum(payload, requestId, batchCount)
    For logicalIndex = 1 To batchCount
        checksum = (checksum * 17 + logicalIndex + workerIndex + requestId) Mod ChecksumModulus
        checksum = (checksum Xor (checksum \ 13)) And &H7FFFFFFF
        checksum = checksum Mod ChecksumModulus
    Next

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
                String(64, Chr(65 + ((requestId + index) Mod 26)))
        keys(index) = kvKey & keySuffix
        kvArgs(index * 2) = keys(index)
        kvArgs(index * 2 + 1) = value
        listValues(index) = value
        zsetArgs(index * 2) = requestId * SampleWidth + index
        zsetArgs(index * 2 + 1) = keySuffix & ":" & value
        fields(index) = "field:" & CStr(index)
        hashArgs(index * 2) = fields(index)
        hashArgs(index * 2 + 1) = value
    Next

    Call redis.Execute("MSET", kvArgs)
    kvReply = redis.Execute("MGET", keys)
    Call redis.Execute("LPUSH", listKey, listValues)
    Call redis.Execute("LTRIM", listKey, 0, 255)
    listReply = redis.Execute("LRANGE", listKey, 0, SampleWidth - 1)
    Call redis.Execute("ZADD", zsetKey, zsetArgs)
    zsetReply = redis.Execute("ZRANGE", zsetKey, 0, SampleWidth - 1, "WITHSCORES")
    Call redis.Execute("HSET", hashKey, hashArgs)
    hashReply = redis.Execute("HMGET", hashKey, fields)
    progress = redis.Execute("INCRBY", prefix & ":progress", batchCount)

    replyItems = 0
    If IsArray(kvReply) Then replyItems = replyItems + UBound(kvReply) + 1
    If IsArray(listReply) Then replyItems = replyItems + UBound(listReply) + 1
    If IsArray(zsetReply) Then replyItems = replyItems + UBound(zsetReply) + 1
    If IsArray(hashReply) Then replyItems = replyItems + UBound(hashReply) + 1
    If (progress Mod 1000000) = 0 Then
        Call redis.Execute("HSET", prefix & ":milestones", CStr(progress), _
                           CStr(workerIndex) & ":" & CStr(requestId) & ":" & CStr(checksum))
    End If
    ExerciseRedis = Array(checksum, progress)
End Function

Dim listener, client, redis, header, payload, responsePacket, result
Dim workerIndex, childPids, port, prefix, requestId, batchCount, payloadLength
Dim opcode, requestsHandled, logicalHandled, replyItems, capturedError, stopWorker, connectionDone

port = CLng(Request.QueryString("port"))
prefix = CStr(Request.QueryString("prefix"))
If port < 1 Or Len(prefix) = 0 Then
    Response.Write "fork-rpc-fail|configuration"
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
        connectionDone = False
        Do While Not connectionDone And Not stopWorker
            header = ReadExact(client, RequestHeaderLength)
            If LenB(header) <> RequestHeaderLength Then
                connectionDone = True
            Else
                opcode = ByteAt(header, 6)
                requestId = DecodeUInt32(header, 7)
                batchCount = DecodeUInt32(header, 11)
                payloadLength = DecodeUInt32(header, 15)
                payload = ReadExact(client, payloadLength)
                If ByteAt(header, 1) = 79 And ByteAt(header, 2) = 0 And _
                   ByteAt(header, 3) = 65 And ByteAt(header, 4) = 82 And _
                   ByteAt(header, 5) = ProtocolVersion And LenB(payload) = payloadLength Then
                    If opcode = OpcodeStop Then
                        responsePacket = "O" & ChrB(0) & "AR" & ChrB(ProtocolVersion) & _
                                         ChrB(ResponseOpcode) & EncodeUInt32(requestId) & _
                                         EncodeUInt32(0) & EncodeUInt32(0) & _
                                         EncodeUInt32(workerIndex) & EncodeUInt32(0)
                        Call client.Write(responsePacket)
                        stopWorker = True
                    ElseIf opcode = OpcodeWork And batchCount > 0 Then
                        result = ExerciseRedis(redis, prefix, workerIndex, requestId, batchCount, payload, replyItems)
                        requestsHandled = requestsHandled + 1
                        logicalHandled = logicalHandled + batchCount
                        responsePacket = "O" & ChrB(0) & "AR" & ChrB(ProtocolVersion) & _
                                         ChrB(ResponseOpcode) & EncodeUInt32(requestId) & _
                                         EncodeUInt32(result(0)) & EncodeUInt32(result(1)) & _
                                         EncodeUInt32(workerIndex) & EncodeUInt32(replyItems)
                        Call client.Write(responsePacket)
                    End If
                End If
            End If
        Loop
        client.Close
    End If
Loop

redis.Close
listener.Close
Response.Write "fork-rpc-worker-ok|" & CStr(workerIndex) & "|" & CStr(requestsHandled) & "|" & _
               CStr(logicalHandled) & "|" & childPids & vbCrLf
%>
