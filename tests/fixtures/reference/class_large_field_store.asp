<%
Function IsBlank(ByVal value)
    IsBlank = False
    If IsNull(value) Then
        IsBlank = True
    Else
        If IsEmpty(value) Or Trim(value) = "" Then IsBlank = True
    End If
End Function

Function IsNumber(value)
    If IsBlank(value) Then
        IsNumber = False
    Else
        IsNumber = IsNumeric(value)
    End If
End Function

Class LargeFieldProbe
    Public Field001, Field002, Field003, Field004, Field005, Field006, Field007, Field008
    Public Field009, Field010, Field011, Field012, Field013, Field014, Field015, Field016
    Public Field017, Field018, Field019, Field020, Field021, Field022, Field023, Field024
    Public Field025, Field026, Field027, Field028, Field029, Field030, Field031, Field032
    Public Field033, Field034, Field035, Field036, Field037, Field038, Field039, Field040
    Public Field041, Field042, Field043, Field044, Field045, Field046, Field047, Field048
    Public Field049, Field050, Field051, Field052, Field053, Field054, Field055, Field056
    Public Field057, Field058, Field059, Field060, Field061, Field062, Field063, Field064
    Public Field065, Field066, Field067, Field068, Field069, Field070, Field071, Field072
    Public Field073, Field074, Field075, Field076, Field077, Field078, Field079, Field080
    Public Field081, Field082, Field083, Field084, Field085, Field086, Field087, Field088
    Public Field089, Field090, Field091, Field092, Field093, Field094, Field095, Field096
    Public Field097, Field098, Field099, Field100, Field101, Field102, Field103, Field104
    Public Field105, Field106, Field107, Field108, Field109, Field110, Field111, Field112
    Public Field113, Field114, Field115, Field116, Field117, Field118, Field119, Field120
    Public Field121, Field122, Field123, Field124, Field125, Field126, Field127, Field128

    Private Sub Class_Initialize()
        Field001 = "first"
        Field064 = "middle"
        Field128 = "last"
    End Sub

    Public Function Snapshot()
        Snapshot = Field001 & ":" & Field064 & ":" & Field128
    End Function

    Public Function Pick(id)
        If IsNumber(id) Then Field002 = "numeric"
        If Field064 = "middle" Then Pick = Snapshot()
    End Function
End Class

Set probe = New LargeFieldProbe
Response.Write probe.Pick(Null)
%>
