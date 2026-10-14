<%
Dim earlyGlobal
earlyGlobal = "stable"
Dim filler01, filler02, filler03, filler04, filler05, filler06, filler07, filler08
Dim filler09, filler10, filler11, filler12, filler13, filler14, filler15, filler16
Dim filler17, filler18, filler19, filler20, filler21, filler22, filler23, filler24
Dim filler25, filler26, filler27, filler28, filler29, filler30, filler31, filler32
Dim filler33, filler34, filler35, filler36, filler37, filler38, filler39, filler40
filler01 = 1
filler02 = 2
filler03 = 3
filler04 = 4
filler05 = 5
filler06 = 6
filler07 = 7
filler08 = 8
filler09 = 9
filler10 = 10
filler11 = 11
filler12 = 12
filler13 = 13
filler14 = 14
filler15 = 15
filler16 = 16
filler17 = 17
filler18 = 18
filler19 = 19
filler20 = 20
filler21 = 21
filler22 = 22
filler23 = 23
filler24 = 24
filler25 = 25
filler26 = 26
filler27 = 27
filler28 = 28
filler29 = 29
filler30 = 30
filler31 = 31
filler32 = 32
filler33 = 33
filler34 = 34
filler35 = 35
filler36 = 36
filler37 = 37
filler38 = 38
filler39 = 39
filler40 = 40

Function ReadEarlyGlobal()
    ReadEarlyGlobal = earlyGlobal
End Function

Class GlobalStoreProbe
    Public Function ReadValue()
        ReadValue = ReadEarlyGlobal()
    End Function
End Class

Set probe = New GlobalStoreProbe
Response.Write probe.ReadValue()
%>
