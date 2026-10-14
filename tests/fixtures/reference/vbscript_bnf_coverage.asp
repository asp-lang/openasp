<%
Class Marker
End Class

Class SafeNames
    Public Default

    Function ReadDefault()
        ReadDefault = Default
    End Function
End Class

Class Box
    Private Const Prefix = "item"

    Public Default Property Get Item(index)
        Item = Prefix & CStr(index)
    End Property

    Property Get Name()
        Name = "box"
    End Property
End Class

Sub ClearArray(ByRef values())
    Erase values
End Sub

Function MissingArgs(a, b, c, d)
    MissingArgs = CStr(IsEmpty(a)) & CStr(IsEmpty(b)) & CStr(IsEmpty(c)) & CStr(IsEmpty(d))
End Function

Sub WriteMissingArgs(a, b, c, d)
    Response.Write CStr(IsEmpty(a)) & CStr(IsEmpty(b)) & CStr(IsEmpty(c)) & CStr(IsEmpty(d))
End Sub

Public Const PublicValue = 7
Public Default
Dim [spaced name]
Dim Erase
Dim items(1)
Dim leftObject
Dim rightObject
Dim boxObject
Dim firstArray
Dim secondArray
Dim safeNamesObject

[spaced name] = +5
Erase = 9
Default = 10
items(0) = "kept"
Call ClearArray(items)
Set leftObject = New Marker
Set rightObject = New Marker
Set boxObject = New Box
Set safeNamesObject = New SafeNames
safeNamesObject.Default = 11
ReDim firstArray(1), secondArray(2)

Response.Write CStr([spaced name])
Response.Write "|" & CStr(&10)
Response.Write "|" & CStr(&H10&)
Response.Write "|" & CStr(1E2)
Response.Write "|" & CStr(.25E2)
Response.Write "|" & CStr(True Eqv False)
Response.Write "|" & CStr(True Imp False)
Response.Write "|" & CStr(leftObject Is Not rightObject)
Response.Write "|" & CStr(IsEmpty(items))
Response.Write "|" & CStr(Year(#12/31/2020#))
Response.Write "|" & CStr(PublicValue)
Response.Write "|" & CStr(UBound(firstArray))
Response.Write "|" & CStr(UBound(secondArray))
Response.Write "|" & boxObject(3)
Response.Write "|" & boxObject.Name
Response.Write "|" & CStr(Erase)
Response.Write "|" & MissingArgs(1,,3,)
Response.Write "|"
WriteMissingArgs ,2,,4
Response.Write "|" & CStr(Default)
Response.Write "|" & CStr(safeNamesObject.ReadDefault())
%>
