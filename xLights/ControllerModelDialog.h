#pragma once

//(*Headers(ControllerModelDialog)
#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
//*)

#include "controllers/ControllerUploadData.h"
#include <wx/prntbase.h>

class ControllerModelDialog;
class Output;
class BaseCMObject;
class xLightsFrame;

class ControllerModelPrintout : public wxPrintout
{
    ControllerModelDialog* _controllerDialog;
public:
    ControllerModelPrintout(ControllerModelDialog* controllerDialog);
    virtual bool OnPrintPage(int pageNum) override;
};

class ControllerModelDialog: public wxDialog
{
	std::string _title;
    UDController* _cud = nullptr;
	Controller* _controller = nullptr;
	ModelManager* _mm = nullptr;
	xLightsFrame* _xLights = nullptr;
	ControllerCaps* _caps = nullptr;
	std::list<BaseCMObject*> _models;
	std::list<BaseCMObject*> _controllers;

	BaseCMObject* GetControllerCMObjectAt(wxPoint mouse);
	BaseCMObject* GetModelsCMObjectAt(wxPoint mouse);
	void ReloadModels();

	public:

		ControllerModelDialog(wxWindow* parent, UDController* cud, ModelManager* mm, Controller* controller, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
		virtual ~ControllerModelDialog();

		//(*Declarations(ControllerModelDialog)
		wxPanel* Panel3;
		wxPanel* Panel4;
		wxPanel* PanelController;
		wxPanel* PanelModels;
		wxScrollBar* ScrollBar_Controller_H;
		wxScrollBar* ScrollBar_Controller_V;
		wxScrollBar* ScrollBar_Models;
		wxSplitterWindow* SplitterWindow1;
		//*)

        static const long CONTROLLERModel_PRINT;
        static const long CONTROLLERModel_SAVE_CSV;

        void RenderPicture(wxBitmap& bitmap, bool printer);
		void DropFromModels(const wxPoint& location, const std::string& name, wxPanel* target);
		void DropFromController(const wxPoint& location, const std::string& name, wxPanel* target);

	protected:

		//(*Identifiers(ControllerModelDialog)
		static const long ID_PANEL1;
		static const long ID_SCROLLBAR1;
		static const long ID_SCROLLBAR2;
		static const long ID_PANEL3;
		static const long ID_PANEL2;
		static const long ID_SCROLLBAR3;
		static const long ID_PANEL4;
		static const long ID_SPLITTERWINDOW1;
		//*)

	private:

		//(*Handlers(ControllerModelDialog)
		void OnPanelControllerLeftDown(wxMouseEvent& event);
		void OnPanelControllerKeyDown(wxKeyEvent& event);
		void OnPanelControllerLeftUp(wxMouseEvent& event);
		void OnPanelControllerLeftDClick(wxMouseEvent& event);
		void OnPanelControllerMouseMove(wxMouseEvent& event);
		void OnPanelControllerMouseEnter(wxMouseEvent& event);
		void OnPanelControllerMouseLeave(wxMouseEvent& event);
		void OnPanelControllerRightDown(wxMouseEvent& event);
		void OnPanelControllerPaint(wxPaintEvent& event);
		void OnPanelControllerResize(wxSizeEvent& event);
		void OnScrollBar_Controller_HScroll(wxScrollEvent& event);
		void OnScrollBar_Controller_HScrollThumbTrack(wxScrollEvent& event);
		void OnScrollBar_Controller_HScrollChanged(wxScrollEvent& event);
		void OnScrollBar_Controller_VScroll(wxScrollEvent& event);
		void OnScrollBar_Controller_VScrollThumbTrack(wxScrollEvent& event);
		void OnScrollBar_Controller_VScrollChanged(wxScrollEvent& event);
		void OnScrollBar_ModelsScroll(wxScrollEvent& event);
		void OnScrollBar_ModelsScrollThumbTrack(wxScrollEvent& event);
		void OnScrollBar_ModelsScrollChanged(wxScrollEvent& event);
		void OnPanelModelsPaint(wxPaintEvent& event);
		void OnPanelModelsKeyDown(wxKeyEvent& event);
		void OnPanelModelsLeftDown(wxMouseEvent& event);
		void OnPanelModelsLeftUp(wxMouseEvent& event);
		void OnPanelModelsLeftDClick(wxMouseEvent& event);
		void OnPanelModelsRightDown(wxMouseEvent& event);
		void OnPanelModelsMouseMove(wxMouseEvent& event);
		void OnPanelModelsMouseWheel(wxMouseEvent& event);
		void OnPanelModelsResize(wxSizeEvent& event);
		void OnPanelModelsMouseEnter(wxMouseEvent& event);
		void OnPanelModelsMouseLeave(wxMouseEvent& event);
		void OnPanelControllerMouseWheel(wxMouseEvent& event);
		//*)

        void OnPopupCommand(wxCommandEvent & event);
		void SaveCSV();

		DECLARE_EVENT_TABLE()
};
