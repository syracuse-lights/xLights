#include "RotoZoom.h"
#include <wx/wx.h>
#include <wx/string.h>

void RotoZoomParms::SetSerialisedValue(std::string k, std::string s)
{
    wxString kk = wxString(k.c_str());
    if (kk == "Id")
    {
        _id = s;
    }
    else if (kk == "Rots")
    {
        _rotations = wxAtof(wxString(s.c_str()));
    }
    else if (kk == "Zooms")
    {
        _zooms = wxAtof(wxString(s.c_str()));
    }
    else if (kk == "ZMax")
    {
        _zoommaximum = wxAtof(wxString(s.c_str()));
    }
    else if (kk == "X")
    {
        _xcenter = wxAtoi(wxString(s.c_str()));
    }
    else if (kk == "Y")
    {
        _ycenter = wxAtoi(wxString(s.c_str()));
    }
    _active = true;
}

RotoZoomParms::RotoZoomParms(const std::string& id, float rotations, float zooms, float zoommaximum, int x, int y)
{
    _active = false;
    _id = id;
    ApplySettings(rotations, zooms, zoommaximum, x, y);
}

void RotoZoomParms::ApplySettings(float rotations, float zooms, float zoommaximum, int x, int y)
{
    _rotations = rotations;
    _zooms = zooms;
    _zoommaximum = zoommaximum;
    _xcenter = x;
    _ycenter = y;
}

void RotoZoomParms::SetDefault(wxSize size)
{
    _rotations = 1;
    _zooms = 1;
    _xcenter = size.GetWidth() / 2;
    _ycenter = size.GetHeight() / 2;
    _active = false;
}

std::string RotoZoomParms::Serialise()
{
    std::string res = "";

    if (IsActive())
    {
        res += "Id=" + _id + "|";
        if (_rotations != 10.0f)
        {
            res += "Rots=" + std::string(wxString::Format("%.2f", _rotations).c_str()) + "|";
        }
        if (_zooms != 10.0f)
        {
            res += "Zooms=" + std::string(wxString::Format("%.2f", _zooms).c_str()) + "|";
        }
        if (_zoommaximum != 20.0f)
        {
            res += "ZMax=" + std::string(wxString::Format("%.2f", _zoommaximum).c_str()) + "|";
        }
        if (_xcenter != 50)
        {
            res += "X=" + std::string(wxString::Format("%d", _xcenter).c_str()) + "|";
        }
        if (_ycenter != 50)
        {
            res += "Y=" + std::string(wxString::Format("%d", _ycenter).c_str()) + "|";
        }
    }
    return res;
}

void RotoZoomParms::Deserialise(std::string s)
{
    if (s == "")
    {
        _active = false;
    }
    else
    {
        _active = true;
        _rotations = 10.0f;
        _zooms = 10.0f;
        _zoommaximum = 20.0f;
        _xcenter = 50;
        _ycenter = 50;
        wxArrayString v = wxSplit(wxString(s.c_str()), '|');
        for (auto vs = v.begin(); vs != v.end(); vs++)
        {
            wxArrayString v1 = wxSplit(*vs, '=');
            if (v1.size() == 2)
            {
                SetSerialisedValue(v1[0].ToStdString(), v1[1].ToStdString());
            }
        }
    }
}

wxPoint RotoZoomParms::GetTransform(float x, float y, float offset, wxSize size)
{
    float PI_2 = 6.283185307179586476925286766559f;
    float angle = PI_2 * _rotations / 10.0f * offset;
    float scale = 1.0f;
    
    if (_zooms != 0)
    {
        scale = (sin(PI_2 * (_zooms / 10.0f) * offset) + 1.0f) / 2.0f * _zoommaximum / 10.0f;
        if (scale < 0.3f)
        {
            scale = 0.3f;
        }
    }

    float xoff = (_xcenter * size.GetWidth()) / 100.0;
    float yoff = (_ycenter * size.GetHeight()) / 100.0;
    float c = cos(-angle);
    float s = sin(-angle);

    float u = xoff + c * (x - xoff) * scale //(1.0f / scale)
        + s * (y - yoff) * scale; //(1.0f / scale);
    float v = yoff + -s * (x - xoff) * scale //(1.0f / scale)
        + c * (y - yoff) * scale; //(1.0f / scale);

    return wxPoint(u, v);
}