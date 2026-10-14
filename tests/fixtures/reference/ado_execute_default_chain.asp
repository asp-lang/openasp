<%
Dim connection
Set connection = Server.CreateObject("ADODB.Connection")
Call connection.Open("Provider=SQLite;Data Source=:memory:")
Call connection.Execute("drop table if exists settings; create table settings(value text); insert into settings values('saved')")

Class SettingsHolder
    Public Value
End Class

Function LoadSetting()
    Dim holder
    Set holder = New SettingsHolder
    holder.Value = connection.Execute("select value from settings")(0)
    LoadSetting = holder.Value
End Function

Response.Write LoadSetting()
%>
