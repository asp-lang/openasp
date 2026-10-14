<%
Dim C_DATABASE
C_DATABASE = "/db/QuickerSite.mdb"

Sub Echo(value)
    Response.Write value
End Sub

Echo "Data Source=" & C_DATABASE
Response.Write "|"

Class Sink
    Public Sub Open(value)
        Response.Write value
    End Sub
End Class

Set target = New Sink
target.Open "Data Source="&C_DATABASE
Response.Write "|"

Dim QS_DBS
QS_DBS = 1

Class DatabaseProbe
    Private connection

    Private Sub Class_Initialize()
        Set connection = Nothing
    End Sub

    Public Function GetConn()
        On Error Resume Next
        If connection Is Nothing Then
            Set connection = New Sink
            Select Case QS_DBS
                Case 1
                    connection.Open "Data Source="&C_DATABASE
            End Select
        End If
        Set GetConn = connection
        On Error Goto 0
    End Function
End Class

Set databaseObject = New DatabaseProbe
Set connectionObject = databaseObject.GetConn()
%>
