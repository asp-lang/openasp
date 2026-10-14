<%
Option Explicit

Dim doc
Dim item
Dim cdata

Set doc = Server.CreateObject("Microsoft.XMLDOM")
Set item = doc.createElement("item")
item.AppendChild(doc.createElement("description"))
Set cdata = doc.createNode("cdatasection", "", "")
cdata.nodeValue = "alpha ]]> beta"
item.selectSingleNode("description").AppendChild(cdata)
doc.AppendChild(item)

Response.Write doc.xml
%>
