#pragma once

class CBlender_SPP : public IBlender  
{
public:
	virtual		const char*		getComment()	{ return "INTERNAL: calc SPP";	}
	virtual		bool		canBeDetailed()	{ return false;	}
	virtual		bool		canBeLMAPped()	{ return false;	}

	virtual		void		Compile			(CBlender_Compile& C);

	CBlender_SPP();
	virtual ~CBlender_SPP();
};

class CBlender_Bodycam final : public IBlender
{
public:
	const char* getComment() override { return "INTERNAL: Bodycam"; }
	bool canBeDetailed() override { return false; }
	bool canBeLMAPped() override { return false; }
	void Compile(CBlender_Compile& C) override;
	CBlender_Bodycam() { description.CLS = 0; }
};
