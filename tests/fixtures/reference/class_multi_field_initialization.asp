<%
Class MultiFieldProbe
    Public Field01, Field02, Field03, Field04, Field05, Field06, Field07, Field08
    Public Field09, Field10, Field11, Field12, Field13, Field14, Field15, Field16
    Public Field17, Field18, Field19, Field20, Field21, Field22, Field23, Field24

    Private Sub Class_Initialize()
        Field01 = "01"
        Field02 = "02"
        Field03 = "03"
        Field04 = "04"
        Field05 = "05"
        Field06 = "06"
        Field07 = "07"
        Field08 = "08"
        Field09 = "09"
        Field10 = "10"
        Field11 = "11"
        Field12 = "12"
        Field13 = "13"
        Field14 = "14"
        Field15 = "15"
        Field16 = "16"
        Field17 = "17"
        Field18 = "18"
        Field19 = "19"
        Field20 = "20"
        Field21 = "21"
        Field22 = "22"
        Field23 = "23"
        Field24 = "24"
    End Sub

    Public Function Snapshot()
        Snapshot = Field01 & ":" & Field08 & ":" & Field16 & ":" & Field24
    End Function
End Class

Set probe = New MultiFieldProbe
Response.Write probe.Snapshot()
%>
