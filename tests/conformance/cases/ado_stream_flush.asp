<%
Dim stream
Set stream = Server.CreateObject("ADODB.Stream")
stream.Open
stream.WriteText "flushed"
stream.Flush
stream.Position = 0
Response.Write stream.ReadText
%>
